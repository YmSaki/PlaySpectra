// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"bufio"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/YmSaki/PlaySpectra/playspectra"
)

// fixtureAdapter is an in-process stand-in for the PlaySpectra adapter: enough
// of the NDJSON control channel that the live probes can run without a Monado
// build. Roles, frame-synchronised ordering and reset semantics are modelled
// because the probes assert on them; every other command answers ok.
type fixtureAdapter struct {
	listener  net.Listener
	mu        sync.Mutex
	writeMu   sync.Mutex
	state     map[string]any
	conns     map[net.Conn]string
	writer    net.Conn
	lastFrame int
	frameZ    float64
	haveFrame bool
	wait      sync.WaitGroup
	stop      chan struct{}
	stopOnce  sync.Once
}

func startFixtureAdapter(t *testing.T) *fixtureAdapter {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	adapter := &fixtureAdapter{
		listener: listener,
		state:    playspectra.DefaultModel().Snapshot(0).Raw(),
		conns:    map[net.Conn]string{},
		stop:     make(chan struct{}),
	}
	adapter.wait.Add(2)
	go adapter.accept()
	go adapter.broadcastHaptics()
	t.Cleanup(adapter.close)
	return adapter
}

func (a *fixtureAdapter) address() string { return a.listener.Addr().String() }
func (a *fixtureAdapter) port() int       { return a.listener.Addr().(*net.TCPAddr).Port }

func (a *fixtureAdapter) close() {
	a.stopOnce.Do(func() {
		close(a.stop)
		_ = a.listener.Close()
		a.mu.Lock()
		for conn := range a.conns {
			_ = conn.Close()
		}
		a.mu.Unlock()
		a.wait.Wait()
	})
}

func (a *fixtureAdapter) accept() {
	defer a.wait.Done()
	for {
		conn, err := a.listener.Accept()
		if err != nil {
			return
		}
		a.mu.Lock()
		a.conns[conn] = ""
		a.mu.Unlock()
		a.wait.Add(1)
		go a.serve(conn)
	}
}

func (a *fixtureAdapter) serve(conn net.Conn) {
	defer a.wait.Done()
	defer func() {
		a.mu.Lock()
		delete(a.conns, conn)
		if a.writer == conn {
			a.writer = nil
		}
		a.mu.Unlock()
		_ = conn.Close()
	}()
	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	for scanner.Scan() {
		var request map[string]any
		if err := json.Unmarshal(scanner.Bytes(), &request); err != nil {
			return
		}
		a.send(conn, a.respond(conn, request))
	}
}

func (a *fixtureAdapter) send(conn net.Conn, message map[string]any) {
	data, err := json.Marshal(message)
	if err != nil {
		return
	}
	a.writeMu.Lock()
	defer a.writeMu.Unlock()
	_, _ = conn.Write(append(data, '\n'))
}

func (a *fixtureAdapter) respond(conn net.Conn, request map[string]any) map[string]any {
	a.mu.Lock()
	defer a.mu.Unlock()
	response := map[string]any{"ok": true, "request_id": request["request_id"]}
	deny := func(reason string) map[string]any {
		response["ok"], response["error_type"], response["error"] = false, "protocol_error", reason
		return response
	}
	switch text(request["cmd"]) {
	case "hello":
		role := text(request["role"])
		if role == "" {
			role = "writer"
		}
		// Every non-observer role writes; the coupling probe says hello as
		// "coupling" and then drives the head, so the fixture cannot key write
		// permission off the literal string "writer".
		if role != "observer" {
			if a.writer != nil && a.writer != conn {
				response["ok"], response["error"] = false, "writer_taken"
				return response
			}
			a.writer = conn
		}
		a.conns[conn] = role
		response["role_granted"] = role
	case "get_state":
		response["state"] = cloneJSONMap(a.state)
	case "set_state":
		if a.writer != conn {
			return deny("not_writer")
		}
		state, _ := request["state"].(map[string]any)
		return a.applyState(response, state)
	case "reset":
		if a.writer != conn {
			return deny("not_writer")
		}
		a.state = playspectra.DefaultModel().Snapshot(0).Raw()
		a.haveFrame = false
	case "status":
		observers := 0
		for _, role := range a.conns {
			if role == "observer" {
				observers++
			}
		}
		response["writer_connected"], response["observers"] = a.writer != nil, observers
	}
	return response
}

// applyState models the adapter's frame-synchronised ordering: a newer logical
// frame is applied, a repeat of the current frame with the same content is
// idempotent, a repeat with different content conflicts, and an older frame is
// stale. Frame content is compared by head z, the only field the frame probes
// vary.
func (a *fixtureAdapter) applyState(response, state map[string]any) map[string]any {
	if state == nil {
		response["applied"] = false
		return response
	}
	clock, _ := state["clock"].(map[string]any)
	logical, framed := number(resolve(clock, "logical_frame"))
	if text(resolve(clock, "mode")) != "frame_synchronized" || !framed {
		mergeState(a.state, state)
		response["applied"] = true
		return response
	}
	frame := int(logical)
	z, _ := number(resolve(state, "hmd", "head", "position", 2))
	switch {
	case !a.haveFrame || frame > a.lastFrame:
		mergeState(a.state, state)
		a.haveFrame, a.lastFrame, a.frameZ = true, frame, z
		response["applied"], response["logical_frame"] = true, frame
	case frame == a.lastFrame && z == a.frameZ:
		response["applied"], response["idempotent"], response["logical_frame"] = true, true, frame
	case frame == a.lastFrame:
		response["ok"], response["error_type"] = false, "conflict_error"
		response["error"], response["logical_frame"] = "frame_content_mismatch", frame
	default:
		response["applied"], response["reason"] = false, "stale_frame"
	}
	return response
}

// broadcastHaptics keeps a haptics event flowing to every connection. The
// multi-observer probe waits for one on three clients at once and there is no
// host application here to trigger it.
func (a *fixtureAdapter) broadcastHaptics() {
	defer a.wait.Done()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-a.stop:
			return
		case <-ticker.C:
			a.mu.Lock()
			conns := make([]net.Conn, 0, len(a.conns))
			for conn := range a.conns {
				conns = append(conns, conn)
			}
			a.mu.Unlock()
			for _, conn := range conns {
				a.send(conn, map[string]any{"event": "haptics", "hand": "right", "amplitude": 0.5})
			}
		}
	}
}

func (a *fixtureAdapter) headPosition() []any {
	a.mu.Lock()
	defer a.mu.Unlock()
	position, _ := resolve(a.state, "hmd", "head", "position").([]any)
	return append([]any(nil), position...)
}

func mergeState(destination, source map[string]any) {
	for key, value := range source {
		nested, isMap := value.(map[string]any)
		existing, wasMap := destination[key].(map[string]any)
		if isMap && wasMap {
			mergeState(existing, nested)
			continue
		}
		destination[key] = value
	}
}

// cloneJSONMap detaches the snapshot handed to a client so that the response is
// marshalled outside the fixture's lock without racing the next writer frame.
func cloneJSONMap(value map[string]any) map[string]any {
	data, err := json.Marshal(value)
	if err != nil {
		return nil
	}
	var out map[string]any
	if err := json.Unmarshal(data, &out); err != nil {
		return nil
	}
	return out
}

// fixtureCapture serves the layer's capture channel, whose protocol is one
// request and one response without request_id echoing. The view pose tracks the
// adapter's head so the coupling probe sees the runtime move.
type fixtureCapture struct {
	listener  net.Listener
	adapter   *fixtureAdapter
	png       string
	dropAxis  func(call int) string
	viewCalls atomic.Int64
	wait      sync.WaitGroup
	stop      chan struct{}
	stopOnce  sync.Once
}

// dropPoseAxis omits one axis from the pose of the nth view response (1 is the
// baseline read, 2 the post-move one) so a probe can be shown a position that
// the runtime never fully reported. Options are applied before the listener is
// served so no configuration races the first client.
func dropPoseAxis(call int, axis string) func(*fixtureCapture) {
	return func(c *fixtureCapture) {
		c.dropAxis = func(n int) string {
			if n == call {
				return axis
			}
			return ""
		}
	}
}

func startFixtureCapture(t *testing.T, adapter *fixtureAdapter, options ...func(*fixtureCapture)) *fixtureCapture {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	png := filepath.Join(t.TempDir(), "capture.png")
	if err := os.WriteFile(png, []byte("\x89PNG\r\n\x1a\nfixture"), 0o600); err != nil {
		t.Fatal(err)
	}
	capture := &fixtureCapture{listener: listener, adapter: adapter, png: png, stop: make(chan struct{})}
	for _, option := range options {
		option(capture)
	}
	capture.wait.Add(1)
	go capture.accept()
	t.Cleanup(capture.close)
	return capture
}

func (c *fixtureCapture) address() string { return c.listener.Addr().String() }
func (c *fixtureCapture) port() int       { return c.listener.Addr().(*net.TCPAddr).Port }

func (c *fixtureCapture) close() {
	c.stopOnce.Do(func() {
		close(c.stop)
		_ = c.listener.Close()
		c.wait.Wait()
	})
}

func (c *fixtureCapture) accept() {
	defer c.wait.Done()
	for {
		conn, err := c.listener.Accept()
		if err != nil {
			return
		}
		c.wait.Add(1)
		go c.serve(conn)
	}
}

func (c *fixtureCapture) serve(conn net.Conn) {
	defer c.wait.Done()
	defer conn.Close()
	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	for scanner.Scan() {
		var request map[string]any
		if err := json.Unmarshal(scanner.Bytes(), &request); err != nil {
			return
		}
		response := map[string]any{"ok": true}
		switch text(request["cmd"]) {
		case "screenshot":
			response["path"] = c.png
		case "view":
			position := c.adapter.headPosition()
			pose := map[string]any{"x": 0.0, "y": 0.0, "z": 0.0}
			if len(position) == 3 {
				pose["x"], pose["y"], pose["z"] = position[0], position[1], position[2]
			}
			if c.dropAxis != nil {
				delete(pose, c.dropAxis(int(c.viewCalls.Add(1))))
			}
			response["views"] = []any{map[string]any{"pose": pose}}
		}
		data, err := json.Marshal(response)
		if err != nil {
			return
		}
		if _, err := conn.Write(append(data, '\n')); err != nil {
			return
		}
	}
}
