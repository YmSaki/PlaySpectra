// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package playspectra

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"testing"
	"time"

	"github.com/YmSaki/PlaySpectra/protocol"
)

func TestServerAgainstNDJSONTCPAdapter(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	done := make(chan error, 1)
	go func() {
		conn, acceptErr := listener.Accept()
		if acceptErr != nil {
			done <- acceptErr
			return
		}
		defer conn.Close()
		state := DefaultModel().Snapshot(0).Raw()
		scanner := bufio.NewScanner(conn)
		for scanner.Scan() {
			var request map[string]any
			if err := json.Unmarshal(scanner.Bytes(), &request); err != nil {
				done <- err
				return
			}
			response := map[string]any{"ok": true, "request_id": request["request_id"]}
			switch request["cmd"] {
			case "hello":
				response["role_granted"] = request["role"]
			case "get_state":
				response["state"] = state
			case "set_state":
				if next, ok := request["state"].(map[string]any); ok {
					state = next
				}
				response["applied"] = true
			case "reset":
				state = DefaultModel().Snapshot(0).Raw()
			}
			data, _ := json.Marshal(response)
			if _, err := fmt.Fprintf(conn, "%s\n", data); err != nil {
				done <- err
				return
			}
		}
		done <- scanner.Err()
	}()
	client, err := protocol.Dial(context.Background(), listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	server := NewServer(client, WithSleeper(func(time.Duration) {}))
	if err := server.Hello(context.Background(), "writer"); err != nil {
		t.Fatal(err)
	}
	if err := server.MoveHead(context.Background(), map[string]any{"position": []any{0, 1.6, -3}}, 0); err != nil {
		t.Fatal(err)
	}
	state, err := server.GetState(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if got := Resolve(state, []any{"hmd", "head", "position", 2}); got.(float64) != -3 {
		t.Fatalf("head z = %v", got)
	}
	_ = client.Close()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
