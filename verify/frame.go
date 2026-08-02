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

func FrameSynchronized(ctx context.Context, address string) (Report, error) {
	report := NewReport("frame-synchronized")
	client, err := protocol.Dial(ctx, address)
	if err != nil {
		return report, err
	}
	defer client.Close()
	hello, err := client.Request(ctx, map[string]any{"cmd": "hello", "request_id": "h", "protocol_version": 1, "role": "writer"})
	if err != nil {
		return report, err
	}
	report.Add("writer hello", requestOK(hello) && text(hello["role_granted"]) == "writer", hello)

	requestNumber := 0
	frame := func(logicalFrame int, z float64) (map[string]any, error) {
		requestNumber++
		return client.Request(ctx, map[string]any{
			"cmd": "set_state", "request_id": fmt.Sprintf("f%d", requestNumber),
			"state": map[string]any{
				"clock": map[string]any{"mode": "frame_synchronized", "logical_frame": logicalFrame},
				"hmd":   map[string]any{"connected": true, "head": pose([]float64{0, 1.6, z})},
			},
		})
	}
	getZ := func(id string) (any, map[string]any, error) {
		response, err := client.Request(ctx, map[string]any{"cmd": "get_state", "request_id": id})
		return resolve(response, "state", "hmd", "head", "position", 2), response, err
	}

	response, err := frame(100, -1)
	if err != nil {
		return report, err
	}
	report.Add("frame100 applied", requestOK(response) && boolean(response["applied"]) && near(response["logical_frame"], 100, 0.001), response)
	z, _, err := getZ("g1")
	if err != nil {
		return report, err
	}
	report.Add("state z=-1.0 after frame100", near(z, -1, 0.001), fmt.Sprintf("z=%v", z))

	response, err = frame(100, -1)
	if err != nil {
		return report, err
	}
	report.Add("frame100 same -> idempotent", requestOK(response) && boolean(response["applied"]) && boolean(response["idempotent"]), response)
	response, err = frame(100, -7.7)
	if err != nil {
		return report, err
	}
	report.Add("frame100 diff -> conflict_error", !requestOK(response) && text(response["error_type"]) == "conflict_error" && text(response["error"]) == "frame_content_mismatch" && near(response["logical_frame"], 100, 0.001), response)
	z, _, err = getZ("g2")
	if err != nil {
		return report, err
	}
	report.Add("conflict did NOT change state (still -1.0)", near(z, -1, 0.001), fmt.Sprintf("z=%v", z))

	response, err = frame(101, -2)
	if err != nil {
		return report, err
	}
	report.Add("frame101 applied", requestOK(response) && boolean(response["applied"]) && near(response["logical_frame"], 101, 0.001), response)
	z, _, err = getZ("g3")
	if err != nil {
		return report, err
	}
	report.Add("state z=-2.0 after frame101", near(z, -2, 0.001), fmt.Sprintf("z=%v", z))
	response, err = frame(100, -1)
	if err != nil {
		return report, err
	}
	report.Add("old frame100 -> stale_frame", requestOK(response) && !boolean(response["applied"]) && text(response["reason"]) == "stale_frame", response)
	z, _, err = getZ("g4")
	if err != nil {
		return report, err
	}
	report.Add("stale did NOT revert state (still -2.0)", near(z, -2, 0.001), fmt.Sprintf("z=%v", z))
	return report, nil
}
