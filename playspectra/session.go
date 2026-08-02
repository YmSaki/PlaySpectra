// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package playspectra

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// SessionRecord is the small, portable process-management record written by
// the CLI. It deliberately stores no shell command string: the executable and
// arguments are kept separately so starting a session never requires a shell.
type SessionRecord struct {
	PID       int       `json:"pid"`
	Command   string    `json:"command"`
	Arguments []string  `json:"arguments,omitempty"`
	StartedAt time.Time `json:"started_at"`
}

func StartSession(pidFile, command string, arguments ...string) (SessionRecord, error) {
	if strings.TrimSpace(command) == "" {
		return SessionRecord{}, errors.New("session start requires a command")
	}
	if record, err := LoadSession(pidFile); err == nil && ProcessRunning(record.PID) {
		return SessionRecord{}, fmt.Errorf("session already running: pid %d", record.PID)
	}
	process := exec.Command(command, arguments...)
	process.Stdout, process.Stderr = os.Stdout, os.Stderr
	if err := process.Start(); err != nil {
		return SessionRecord{}, fmt.Errorf("start session: %w", err)
	}
	record := SessionRecord{PID: process.Process.Pid, Command: command, Arguments: append([]string(nil), arguments...), StartedAt: time.Now().UTC()}
	if err := SaveSession(pidFile, record); err != nil {
		_ = process.Process.Kill()
		return SessionRecord{}, err
	}
	return record, nil
}

func SaveSession(pidFile string, record SessionRecord) error {
	data, err := json.MarshalIndent(record, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(pidFile, append(data, '\n'), 0o644); err != nil {
		return fmt.Errorf("write session file: %w", err)
	}
	return nil
}

func LoadSession(pidFile string) (SessionRecord, error) {
	data, err := os.ReadFile(pidFile)
	if err != nil {
		return SessionRecord{}, fmt.Errorf("read session file: %w", err)
	}
	var record SessionRecord
	if err := json.Unmarshal(data, &record); err != nil {
		return SessionRecord{}, fmt.Errorf("decode session file: %w", err)
	}
	if record.PID <= 0 {
		return SessionRecord{}, errors.New("session file has invalid pid")
	}
	return record, nil
}

func StopSession(pidFile string) error {
	record, err := LoadSession(pidFile)
	if err != nil {
		return err
	}
	process, findErr := os.FindProcess(record.PID)
	if findErr == nil && ProcessRunning(record.PID) {
		if killErr := process.Kill(); killErr != nil && !errors.Is(killErr, os.ErrProcessDone) {
			return fmt.Errorf("stop session: %w", killErr)
		}
	}
	if err := os.Remove(pidFile); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("remove session file: %w", err)
	}
	return nil
}

func ProcessRunning(pid int) bool {
	if pid <= 0 {
		return false
	}
	if runtime.GOOS == "windows" {
		output, err := exec.Command("tasklist", "/FI", "PID eq "+strconv.Itoa(pid), "/FO", "CSV", "/NH").CombinedOutput()
		return err == nil && strings.Contains(string(output), strconv.Itoa(pid))
	}
	process, err := os.FindProcess(pid)
	if err != nil {
		return false
	}
	return process.Signal(syscall.Signal(0)) == nil
}

func SessionStatus(pidFile string) (map[string]any, error) {
	record, err := LoadSession(pidFile)
	if err != nil {
		return map[string]any{"running": false, "error": err.Error()}, nil
	}
	return map[string]any{"running": ProcessRunning(record.PID), "session": record}, nil
}
