// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"context"
	"fmt"

	"github.com/YmSaki/PlaySpectra/protocol"
)

func Reset(ctx context.Context, address string) (Report, error) {
	report := NewReport("reset")
	writer, err := protocol.Dial(ctx, address)
	if err != nil {
		return report, err
	}
	defer writer.Close()
	hello, err := writer.Request(ctx, map[string]any{"cmd": "hello", "request_id": "h", "protocol_version": 1, "role": "writer"})
	if err != nil {
		return report, err
	}
	report.Add("writer hello", requestOK(hello) && text(hello["role_granted"]) == "writer", hello)
	getState := func(client *protocol.Client, id string) (map[string]any, error) {
		return client.Request(ctx, map[string]any{"cmd": "get_state", "request_id": id})
	}
	headZ := func(state map[string]any) any { return resolve(state, "state", "hmd", "head", "position", 2) }
	gripX := func(state map[string]any, hand string) any {
		return resolve(state, "state", hand, "grip", "position", 0)
	}
	trigger := func(state map[string]any, hand string) any {
		return resolve(state, "state", hand, "inputs", "/input/trigger/value")
	}
	state, err := getState(writer, "g0")
	if err != nil {
		return report, err
	}
	report.Add("baseline head z=0.0", near(headZ(state), 0, 0.001), fmt.Sprintf("z=%v", headZ(state)))
	report.Add("baseline left grip x=-0.2", near(gripX(state, "left"), -0.2, 0.001), fmt.Sprintf("x=%v", gripX(state, "left")))
	report.Add("baseline right grip x=0.2", near(gripX(state, "right"), 0.2, 0.001), fmt.Sprintf("x=%v", gripX(state, "right")))
	report.Add("baseline left trigger=0", near(trigger(state, "left"), 0, 0.001), fmt.Sprintf("t=%v", trigger(state, "left")))

	response, err := writer.Request(ctx, map[string]any{"cmd": "set_state", "request_id": "s1", "state": map[string]any{
		"hmd":  map[string]any{"connected": true, "head": pose([]float64{0, 1.6, -2.5})},
		"left": map[string]any{"connected": true, "grip": pose([]float64{-1, 1.3, -0.5}), "aim": pose([]float64{-1, 1.3, -0.5}), "inputs": map[string]any{"/input/trigger/value": 0.9}},
	}})
	if err != nil {
		return report, err
	}
	report.Add("set_state applied", requestOK(response) && boolean(response["applied"]), response)
	state, err = getState(writer, "g1")
	if err != nil {
		return report, err
	}
	report.Add("moved head z=-2.5", near(headZ(state), -2.5, 0.001), fmt.Sprintf("z=%v", headZ(state)))
	report.Add("moved left grip x=-1.0", near(gripX(state, "left"), -1, 0.001), fmt.Sprintf("x=%v", gripX(state, "left")))
	report.Add("moved left trigger=0.9", near(trigger(state, "left"), 0.9, 0.01), fmt.Sprintf("t=%v", trigger(state, "left")))

	response, err = writer.Request(ctx, map[string]any{"cmd": "set_state", "request_id": "s2", "state": map[string]any{
		"clock": map[string]any{"mode": "frame_synchronized", "logical_frame": 200},
		"hmd":   map[string]any{"connected": true, "head": pose([]float64{0, 1.6, -3.3})},
	}})
	if err != nil {
		return report, err
	}
	report.Add("frame200 applied", requestOK(response) && boolean(response["applied"]) && near(response["logical_frame"], 200, 0.001), response)

	observer, err := protocol.Dial(ctx, address)
	if err != nil {
		return report, err
	}
	defer observer.Close()
	observerHello, err := observer.Request(ctx, map[string]any{"cmd": "hello", "request_id": "ho", "protocol_version": 1, "role": "observer"})
	if err != nil {
		return report, err
	}
	report.Add("observer hello", requestOK(observerHello) && text(observerHello["role_granted"]) == "observer", observerHello)
	observerReset, err := observer.Request(ctx, map[string]any{"cmd": "reset", "request_id": "ro1"})
	if err != nil {
		return report, err
	}
	report.Add("observer reset -> not_writer", !requestOK(observerReset) && text(observerReset["error_type"]) == "protocol_error" && text(observerReset["error"]) == "not_writer", observerReset)

	response, err = writer.Request(ctx, map[string]any{"cmd": "reset", "request_id": "r1"})
	if err != nil {
		return report, err
	}
	report.Add("writer reset ok", requestOK(response), response)
	state, err = getState(writer, "g2")
	if err != nil {
		return report, err
	}
	report.Add("reset head -> z=0.0", near(headZ(state), 0, 0.001), fmt.Sprintf("z=%v", headZ(state)))
	report.Add("reset left grip -> x=-0.2", near(gripX(state, "left"), -0.2, 0.001), fmt.Sprintf("x=%v", gripX(state, "left")))
	report.Add("reset right grip -> x=0.2", near(gripX(state, "right"), 0.2, 0.001), fmt.Sprintf("x=%v", gripX(state, "right")))
	report.Add("reset left trigger -> 0", near(trigger(state, "left"), 0, 0.001), fmt.Sprintf("t=%v", trigger(state, "left")))
	observerState, err := getState(observer, "go1")
	if err != nil {
		return report, err
	}
	report.Add("observer sees reset head z=0.0", near(headZ(observerState), 0, 0.001), fmt.Sprintf("z=%v", headZ(observerState)))

	response, err = writer.Request(ctx, map[string]any{"cmd": "set_state", "request_id": "s3", "state": map[string]any{
		"clock": map[string]any{"mode": "frame_synchronized", "logical_frame": 5},
		"hmd":   map[string]any{"connected": true, "head": pose([]float64{0, 1.6, -0.7})},
	}})
	if err != nil {
		return report, err
	}
	report.Add("post-reset old frame5 applied (not stale)", requestOK(response) && boolean(response["applied"]) && near(response["logical_frame"], 5, 0.001), response)
	state, err = getState(writer, "g3")
	if err != nil {
		return report, err
	}
	report.Add("post-reset frame5 moved head z=-0.7", near(headZ(state), -0.7, 0.001), fmt.Sprintf("z=%v", headZ(state)))
	return report, nil
}
