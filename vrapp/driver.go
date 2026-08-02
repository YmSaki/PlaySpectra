// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Package vrapp drives VRAppDummyGame through its line-oriented [VRTEST]
// stdin/stdout contract.
package vrapp

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

const eventPrefix = "[VRTEST] "

type Event map[string]any
type Predicate func(Event) bool

type Config struct {
	Executable string
	Arguments  []string
	Env        []string
	LogPath    string
}

type App struct {
	Config

	cmd     *exec.Cmd
	stdin   io.WriteCloser
	done    chan struct{}
	waitErr error
	events  []Event
	raw     []string
	mu      sync.RWMutex
	write   sync.Mutex
	reqID   uint64
}

func DefaultExecutable() string {
	if path := os.Getenv("PLAYSPECTRA_VRAPP_EXE"); path != "" {
		return path
	}
	workingDirectory, err := os.Getwd()
	if err != nil {
		return filepath.Join("VRAppDummyGame", "build", "vrapp.console.exe")
	}
	return filepath.Join(filepath.Dir(workingDirectory), "VRAppDummyGame", "build", "vrapp.console.exe")
}

func New(config Config) *App {
	if config.Executable == "" {
		config.Executable = DefaultExecutable()
	}
	if config.Env == nil {
		config.Env = os.Environ()
	}
	return &App{Config: config}
}

func (a *App) Start() error {
	if a.cmd != nil {
		return errors.New("VRApp is already started")
	}
	a.cmd = exec.Command(a.Executable, a.Arguments...)
	a.cmd.Env = append([]string(nil), a.Env...)
	stdin, err := a.cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("VRApp stdin: %w", err)
	}
	stdout, err := a.cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("VRApp stdout: %w", err)
	}
	a.cmd.Stderr = a.cmd.Stdout
	a.stdin = stdin
	if err := a.cmd.Start(); err != nil {
		a.cmd = nil
		return fmt.Errorf("start VRApp: %w", err)
	}
	a.done = make(chan struct{})
	go a.readLoop(stdout)
	go func() {
		err := a.cmd.Wait()
		a.mu.Lock()
		a.waitErr = err
		a.mu.Unlock()
		close(a.done)
	}()
	return nil
}

func (a *App) Stop() error {
	if a.cmd == nil {
		return nil
	}
	if a.stdin != nil {
		_ = a.stdin.Close()
	}
	if a.cmd.Process != nil {
		_ = a.cmd.Process.Kill()
	}
	select {
	case <-a.done:
	case <-time.After(3 * time.Second):
	}
	// The Windows console wrapper can leave the GUI-subsystem child alive.
	if runtime.GOOS == "windows" && filepath.Base(a.Executable) == "vrapp.console.exe" {
		for _, name := range []string{"vrapp.exe", "vrapp.console.exe"} {
			_ = exec.Command("taskkill", "/F", "/IM", name).Run()
		}
	}
	if a.LogPath != "" {
		a.mu.RLock()
		log := strings.Join(a.raw, "\n")
		a.mu.RUnlock()
		if err := os.WriteFile(a.LogPath, []byte(log), 0o644); err != nil {
			return fmt.Errorf("write VRApp log: %w", err)
		}
	}
	a.cmd = nil
	return nil
}

func (a *App) readLoop(reader io.Reader) {
	scanner := bufio.NewScanner(reader)
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	for scanner.Scan() {
		a.consumeLine(strings.TrimSuffix(scanner.Text(), "\r"))
	}
}

func (a *App) consumeLine(line string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.raw = append(a.raw, line)
	if !strings.HasPrefix(line, eventPrefix) {
		return
	}
	var event Event
	if err := json.Unmarshal([]byte(strings.TrimPrefix(line, eventPrefix)), &event); err == nil {
		a.events = append(a.events, event)
	}
}

func (a *App) Count() int {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return len(a.events)
}

func (a *App) WaitFor(ctx context.Context, predicate Predicate, since int) Event {
	if ctx == nil {
		ctx = context.Background()
	}
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		a.mu.RLock()
		start := since
		if start < 0 {
			start = 0
		}
		if start > len(a.events) {
			start = len(a.events)
		}
		snapshot := append([]Event(nil), a.events[start:]...)
		a.mu.RUnlock()
		for _, event := range snapshot {
			if predicate(event) {
				return event
			}
		}
		select {
		case <-ctx.Done():
			return nil
		case <-a.done:
			return nil
		case <-ticker.C:
		}
	}
}

func (a *App) NonVRTestLines(limit int) []string {
	a.mu.RLock()
	defer a.mu.RUnlock()
	if limit <= 0 {
		limit = 25
	}
	lines := make([]string, 0, min(limit, len(a.raw)))
	for _, line := range a.raw {
		if !strings.HasPrefix(line, eventPrefix) {
			lines = append(lines, line)
			if len(lines) == limit {
				break
			}
		}
	}
	return lines
}

func (a *App) Request(ctx context.Context, request, wantType string, params map[string]any) Event {
	a.write.Lock()
	defer a.write.Unlock()
	a.reqID++
	id := a.reqID
	message := map[string]any{"req": request, "id": id}
	for key, value := range params {
		message[key] = value
	}
	since := a.Count()
	data, err := json.Marshal(message)
	if err != nil || a.stdin == nil {
		return nil
	}
	if _, err := a.stdin.Write(append(data, '\n')); err != nil {
		return nil
	}
	return a.WaitFor(ctx, func(event Event) bool {
		return text(event["t"]) == wantType && integer(event["id"]) == int64(id)
	}, since)
}

func (a *App) Pose(ctx context.Context) Event {
	return a.Request(ctx, "pose", "pose_snapshot", nil)
}

func (a *App) WaitXRInit(ctx context.Context) Event {
	return a.WaitFor(ctx, func(event Event) bool { return text(event["t"]) == "xr_init" }, 0)
}

func (a *App) WaitControllerActive(ctx context.Context, hand string) Event {
	return a.WaitFor(ctx, func(event Event) bool {
		active, _ := event["active"].(bool)
		return text(event["t"]) == "controller_state" && text(event["hand"]) == hand && active
	}, 0)
}

func AppRevision(executable string) string {
	directory := filepath.Dir(filepath.Dir(executable))
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	revisionBytes, err := exec.CommandContext(ctx, "git", "-C", directory, "rev-parse", "--short", "HEAD").Output()
	if err != nil {
		return ""
	}
	revision := strings.TrimSpace(string(revisionBytes))
	if revision == "" {
		return ""
	}
	dirtyBytes, err := exec.CommandContext(ctx, "git", "-C", directory, "status", "--porcelain").Output()
	if err != nil {
		return revision
	}
	dirty := 0
	for _, line := range strings.Split(string(dirtyBytes), "\n") {
		if strings.TrimSpace(line) != "" {
			dirty++
		}
	}
	if dirty > 0 {
		return fmt.Sprintf("%s +%d dirty", revision, dirty)
	}
	return revision
}

func StageToGlobal(stage []float64, origin Event) []float64 {
	return translate(stage, origin, 1)
}

func GlobalToStage(global []float64, origin Event) []float64 {
	return translate(global, origin, -1)
}

func translate(position []float64, origin Event, direction float64) []float64 {
	result := append([]float64(nil), position...)
	originPosition := floats(origin["pos"])
	for i := 0; i < min(3, min(len(result), len(originPosition))); i++ {
		result[i] += direction * originPosition[i]
	}
	return result
}

func Testbed(event string, values map[string]any) Predicate {
	return func(candidate Event) bool {
		if text(candidate["t"]) != "testbed" || text(candidate["event"]) != event {
			return false
		}
		for key, value := range values {
			if !equal(candidate[key], value) {
				return false
			}
		}
		return true
	}
}

func HoverStart(target, source string) Predicate {
	return Testbed("hover_start", map[string]any{"target": target, "source": source})
}

func Axis(hand, name string) Predicate {
	return func(event Event) bool {
		return text(event["t"]) == "axis" && text(event["hand"]) == hand && text(event["name"]) == name
	}
}

func Button(hand, state string) Predicate {
	return func(event Event) bool {
		return text(event["t"]) == "button" && text(event["hand"]) == hand && text(event["state"]) == state
	}
}

func text(value any) string { result, _ := value.(string); return result }

func integer(value any) int64 {
	switch number := value.(type) {
	case float64:
		return int64(number)
	case int:
		return int64(number)
	case int64:
		return number
	default:
		return -1
	}
}

func floats(value any) []float64 {
	switch values := value.(type) {
	case []float64:
		return values
	case []any:
		result := make([]float64, 0, len(values))
		for _, value := range values {
			if number, ok := value.(float64); ok {
				result = append(result, number)
			}
		}
		return result
	default:
		return nil
	}
}

func equal(a, b any) bool {
	left, leftErr := json.Marshal(a)
	right, rightErr := json.Marshal(b)
	return leftErr == nil && rightErr == nil && bytesEqual(left, right)
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
