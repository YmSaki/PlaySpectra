// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package protocol

import (
	"bufio"
	"context"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"
)

func TestClientMatchesRequestIDAndSkipsEvents(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	done := make(chan error, 1)
	go func() {
		reader := bufio.NewReader(server)
		if _, err := reader.ReadString('\n'); err != nil {
			done <- err
			return
		}
		_, err := server.Write([]byte(`{"event":"haptics"}` + "\n" + `{"request_id":"r1","ok":true}` + "\n"))
		done <- err
	}()
	response, err := client.Request(context.Background(), map[string]any{"cmd": "get_state", "request_id": "r1"})
	if err != nil {
		t.Fatal(err)
	}
	if response["ok"] != true {
		t.Fatalf("response = %v", response)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestClientRequestLineDoesNotRequireRequestID(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	go func() {
		reader := bufio.NewReader(server)
		_, _ = reader.ReadString('\n')
		_, _ = server.Write([]byte(`{"ok":true,"path":"capture.png"}` + "\n"))
	}()
	response, err := client.RequestLine(context.Background(), map[string]any{"cmd": "screenshot"})
	if err != nil || response["path"] != "capture.png" {
		t.Fatalf("response=%v err=%v", response, err)
	}
}

func TestClientHandlesSplitPacketsBlankLinesAndMismatchedReplies(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	done := make(chan error, 1)
	go func() {
		reader := bufio.NewReader(server)
		if _, err := reader.ReadString('\n'); err != nil {
			done <- err
			return
		}
		parts := []string{
			"\n", `{"request_id":"old","ok":true}` + "\n", `{"event":"haptics"}` + "\n",
			`{"request_id":"wanted",`, `"ok":true,"state":{"sequence":7}}` + "\n",
		}
		for _, part := range parts {
			if _, err := io.WriteString(server, part); err != nil {
				done <- err
				return
			}
		}
		done <- nil
	}()
	response, err := client.Request(context.Background(), map[string]any{"cmd": "get_state", "request_id": "wanted"})
	if err != nil {
		t.Fatal(err)
	}
	if response["request_id"] != "wanted" || response["ok"] != true {
		t.Fatalf("response = %v", response)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestRequestConsumesPendingSendOnlyReply(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	done := make(chan error, 1)
	go func() {
		server, err := listener.Accept()
		if err != nil {
			done <- err
			return
		}
		defer server.Close()
		reader := bufio.NewReader(server)
		if _, err := reader.ReadString('\n'); err != nil {
			done <- err
			return
		}
		if _, err := io.WriteString(server, `{"request_id":"frame-1","ok":true}`+"\n"); err != nil {
			done <- err
			return
		}
		if _, err := reader.ReadString('\n'); err != nil {
			done <- err
			return
		}
		if _, err := io.WriteString(server, `{"request_id":"state-1","ok":true,"state":{}}`+"\n"); err != nil {
			done <- err
			return
		}
		done <- nil
	}()
	clientConn, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer clientConn.Close()
	client := NewClient(clientConn)
	if err := client.SendOnly(context.Background(), map[string]any{"cmd": "set_state", "request_id": "frame-1"}); err != nil {
		t.Fatal(err)
	}
	response, err := client.Request(context.Background(), map[string]any{"cmd": "get_state", "request_id": "state-1"})
	if err != nil || response["request_id"] != "state-1" {
		t.Fatalf("response=%v err=%v", response, err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestClientReportsMalformedTruncatedOversizeAndTimeoutResponses(t *testing.T) {
	tests := []struct {
		name      string
		write     string
		close     bool
		timeout   time.Duration
		wantError string
	}{
		{name: "malformed JSON", write: "not-json\n", wantError: "decode NDJSON response"},
		{name: "truncated response", write: `{"request_id":"r"`, close: true, wantError: io.ErrUnexpectedEOF.Error()},
		{name: "oversize response", write: `{"padding":"` + strings.Repeat("x", MaxMessageSize) + `"}` + "\n", wantError: "response exceeds"},
		{name: "context timeout", timeout: 20 * time.Millisecond, wantError: "read NDJSON response"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			server, clientConn := net.Pipe()
			client := NewClient(clientConn)
			defer client.Close()
			go func() {
				reader := bufio.NewReader(server)
				_, _ = reader.ReadString('\n')
				if test.write != "" {
					_, _ = io.WriteString(server, test.write)
				}
				if test.close {
					_ = server.Close()
				}
			}()
			ctx := context.Background()
			if test.timeout > 0 {
				var cancel context.CancelFunc
				ctx, cancel = context.WithTimeout(ctx, test.timeout)
				defer cancel()
			}
			_, err := client.Request(ctx, map[string]any{"cmd": "get_state", "request_id": "r"})
			if err == nil || !strings.Contains(err.Error(), test.wantError) {
				t.Fatalf("error = %v, want substring %q", err, test.wantError)
			}
			if test.close && !errors.Is(err, io.ErrUnexpectedEOF) {
				t.Fatalf("truncated error does not wrap unexpected EOF: %v", err)
			}
			_ = server.Close()
		})
	}
}

func TestClientRejectsOversizeRequestBeforeWriting(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	err := client.SendOnly(context.Background(), map[string]any{"payload": strings.Repeat("x", MaxMessageSize)})
	if err == nil || !strings.Contains(err.Error(), "request exceeds") {
		t.Fatalf("error = %v", err)
	}
}

func TestWaitEventReturnsEventQueuedDuringRequest(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	go func() {
		reader := bufio.NewReader(server)
		_, _ = reader.ReadString('\n')
		_, _ = io.WriteString(server, `{"event":"haptics","hand":"left"}`+"\n"+`{"request_id":"r1","ok":true}`+"\n")
	}()
	if _, err := client.Request(context.Background(), map[string]any{"cmd": "status", "request_id": "r1"}); err != nil {
		t.Fatal(err)
	}
	event, err := client.WaitEvent(context.Background(), "haptics")
	if err != nil || event["hand"] != "left" {
		t.Fatalf("event=%v err=%v", event, err)
	}
}

func TestWaitEventReadsLiveEventAndHonorsContext(t *testing.T) {
	server, clientConn := net.Pipe()
	client := NewClient(clientConn)
	go func() { _, _ = io.WriteString(server, `{"event":"haptics","hand":"right"}`+"\n") }()
	event, err := client.WaitEvent(context.Background(), "haptics")
	if err != nil || event["hand"] != "right" {
		t.Fatalf("event=%v err=%v", event, err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := client.WaitEvent(ctx, "never"); err == nil {
		t.Fatal("WaitEvent ignored context timeout")
	}
	_ = server.Close()
}
