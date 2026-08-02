// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/YmSaki/PlaySpectra/protocol"
)

func MultiObserver(ctx context.Context, address string, hapticsTimeout time.Duration) (Report, error) {
	report := NewReport("multi-observer")
	if hapticsTimeout <= 0 {
		hapticsTimeout = 12 * time.Second
	}
	clients := make([]*protocol.Client, 4)
	for index := range clients {
		client, err := protocol.Dial(ctx, address)
		if err != nil {
			for _, opened := range clients {
				if opened != nil {
					_ = opened.Close()
				}
			}
			return report, err
		}
		clients[index] = client
		defer client.Close()
	}
	obs1, obs2, writer, writer2 := clients[0], clients[1], clients[2], clients[3]
	hello := func(client *protocol.Client, id, role string) (map[string]any, error) {
		return client.Request(ctx, map[string]any{"cmd": "hello", "request_id": id, "protocol_version": 1, "role": role})
	}
	first, err := hello(obs1, "o1", "observer")
	if err != nil {
		return report, err
	}
	second, err := hello(obs2, "o2", "observer")
	if err != nil {
		return report, err
	}
	report.Add("obs1 hello ok", requestOK(first) && text(first["role_granted"]) == "observer", first)
	report.Add("obs2 hello ok", requestOK(second) && text(second["role_granted"]) == "observer", second)
	writerHello, err := hello(writer, "w1", "writer")
	if err != nil {
		return report, err
	}
	report.Add("writer hello ok", requestOK(writerHello) && text(writerHello["role_granted"]) == "writer", writerHello)
	secondWriter, err := hello(writer2, "w2", "writer")
	if err != nil {
		return report, err
	}
	report.Add("2nd writer -> writer_taken", !requestOK(secondWriter) && text(secondWriter["error"]) == "writer_taken", secondWriter)

	observerSet, err := obs1.Request(ctx, map[string]any{
		"cmd": "set_state", "request_id": "os1",
		"state": map[string]any{"sequence": 1, "clock": map[string]any{"mode": "realtime"}, "hmd": map[string]any{"connected": true, "head": pose([]float64{0, 1.6, -1})}},
	})
	if err != nil {
		return report, err
	}
	report.Add("observer set_state -> not_writer", !requestOK(observerSet) && text(observerSet["error"]) == "not_writer", observerSet)
	before, err := obs1.Request(ctx, map[string]any{"cmd": "get_state", "request_id": "g0"})
	if err != nil {
		return report, err
	}
	writerSet, err := writer.Request(ctx, map[string]any{
		"cmd": "set_state", "request_id": "s1",
		"state": map[string]any{"sequence": 5, "clock": map[string]any{"mode": "realtime"}, "hmd": map[string]any{"connected": true, "head": pose([]float64{0, 1.6, -2.5})}},
	})
	if err != nil {
		return report, err
	}
	report.Add("writer set_state applied", requestOK(writerSet) && boolean(writerSet["applied"]), writerSet)
	after, err := obs1.Request(ctx, map[string]any{"cmd": "get_state", "request_id": "g1"})
	if err != nil {
		return report, err
	}
	z := resolve(after, "state", "hmd", "head", "position", 2)
	report.Add("observer get_state reflects writer", near(z, -2.5, 0.001), fmt.Sprintf("z_after=%v before=%v", z, resolve(before, "state", "hmd", "head", "position")))
	status, err := obs2.Request(ctx, map[string]any{"cmd": "status", "request_id": "q1"})
	if err != nil {
		return report, err
	}
	report.Add("status writer_connected+observers", boolean(status["writer_connected"]) && near(status["observers"], 2, 0.001), status)

	eventClients := []*protocol.Client{obs1, obs2, writer}
	events := make([]map[string]any, len(eventClients))
	errors := make([]error, len(eventClients))
	var wait sync.WaitGroup
	for index, client := range eventClients {
		wait.Add(1)
		go func(index int, client *protocol.Client) {
			defer wait.Done()
			eventContext, cancel := context.WithTimeout(ctx, hapticsTimeout)
			defer cancel()
			events[index], errors[index] = client.WaitEvent(eventContext, "haptics")
		}(index, client)
	}
	wait.Wait()
	for index, name := range []string{"obs1", "obs2", "writer"} {
		detail := any(events[index])
		if errors[index] != nil {
			detail = errors[index]
		}
		report.Add("haptics broadcast to "+name, errors[index] == nil && text(events[index]["event"]) == "haptics", detail)
	}
	return report, nil
}
