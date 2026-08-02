// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strconv"

	mcpfrontend "github.com/YmSaki/PlaySpectra/mcp"
)

func MCP(ctx context.Context, executable, host string, operatePort, capturePort int) (Report, error) {
	report := NewReport("mcp")
	client, err := mcpfrontend.StartProcessClient(ctx, executable, []string{
		"mcp", "--host", host, "--port", strconv.Itoa(operatePort), "--capture-port", strconv.Itoa(capturePort),
	}, os.Environ())
	if err != nil {
		return report, err
	}
	defer client.Close()
	if _, err := client.Initialize(ctx); err != nil {
		return report, err
	}
	tools, err := client.ListTools(ctx)
	if err != nil {
		return report, err
	}
	names := map[string]bool{}
	for _, tool := range tools {
		names[text(tool["name"])] = true
	}
	needed := []string{"move_head", "look", "walk_forward", "strafe", "press", "set_trigger", "move_controller", "set_input", "get_state", "screenshot", "reset", "wait_for", "run_scenario"}
	allListed := true
	for _, name := range needed {
		allListed = allListed && names[name]
	}
	report.Add("tools listed", allListed, fmt.Sprintf("%v", names))

	call := func(name string, arguments map[string]any) (map[string]any, error) {
		return client.CallTool(ctx, name, arguments)
	}
	stateFrom := func(result map[string]any) map[string]any {
		var state map[string]any
		_ = json.Unmarshal([]byte(toolText(result)), &state)
		return state
	}
	result, err := call("move_head", map[string]any{"x": 0.0, "y": 1.6, "z": -1.5})
	if err != nil {
		return report, err
	}
	state := stateFrom(result)
	z := resolve(state, "hmd", "head", "position", 2)
	report.Add("move_head -> z=-1.5", near(z, -1.5, 0.05), fmt.Sprintf("z=%v", z))
	result, err = call("look", map[string]any{"yaw_deg": 90.0})
	if err != nil {
		return report, err
	}
	state = stateFrom(result)
	qy := resolve(state, "hmd", "head", "orientation", 1)
	report.Add("look 90deg -> quat.y~0.707", near(qy, 0.7071, 0.05), fmt.Sprintf("qy=%v", qy))
	result, err = call("get_state", map[string]any{})
	if err != nil {
		return report, err
	}
	state = stateFrom(result)
	z = resolve(state, "hmd", "head", "position", 2)
	report.Add("get_state reflects state", near(z, -1.5, 0.05), fmt.Sprintf("z=%v", z))
	result, err = call("screenshot", map[string]any{"eye": "left"})
	if err != nil {
		return report, err
	}
	report.Add("screenshot returns an image", containsContentType(result, "image"), fmt.Sprintf("content=%v", contentTypes(result)))

	scenario, _ := json.Marshal(map[string]any{"name": "mcp_inline", "steps": []any{
		map[string]any{"cmd": "move_head", "to": map[string]any{"position": []float64{0, 1.6, -0.5}}, "duration_ms": 200},
		map[string]any{"cmd": "assert", "get": []any{"hmd", "head", "position", 2}, "op": "near", "value": -0.5, "tol": 0.05},
	}})
	result, err = call("run_scenario", map[string]any{"scenario_json": string(scenario)})
	if err != nil {
		return report, err
	}
	var summary map[string]any
	_ = json.Unmarshal([]byte(toolText(result)), &summary)
	report.Add("run_scenario assert passes", boolean(summary["ok"]) && near(summary["passed"], 1, 0.001), summary)

	if _, err := call("move_controller", map[string]any{"hand": "right", "x": 0.7, "y": 1.1, "z": -0.3}); err != nil {
		return report, err
	}
	result, err = call("get_state", map[string]any{})
	if err != nil {
		return report, err
	}
	state = stateFrom(result)
	gripX := resolve(state, "right", "grip", "position", 0)
	gripY := resolve(state, "right", "grip", "position", 1)
	report.Add("move_controller -> right grip (x/y/z args -> position)", near(gripX, 0.7, 0.05) && near(gripY, 1.1, 0.05), fmt.Sprintf("grip=%v", resolve(state, "right", "grip", "position")))
	if _, err := call("set_input", map[string]any{"hand": "left", "path": "/input/squeeze/value", "value": 0.9}); err != nil {
		return report, err
	}
	result, err = call("get_state", map[string]any{})
	if err != nil {
		return report, err
	}
	state = stateFrom(result)
	value := resolve(state, "left", "inputs", "/input/squeeze/value")
	report.Add("set_input -> left squeeze 0.9", near(value, 0.9, 0.05), fmt.Sprintf("v=%v", value))
	if _, err := call("set_trigger", map[string]any{"hand": "right", "value": 0.8}); err != nil {
		return report, err
	}
	result, err = call("get_state", map[string]any{})
	if err != nil {
		return report, err
	}
	state = stateFrom(result)
	value = resolve(state, "right", "inputs", "/input/trigger/value")
	report.Add("set_trigger -> right trigger 0.8", near(value, 0.8, 0.05), fmt.Sprintf("v=%v", value))
	pathJSON, _ := json.Marshal([]any{"right", "inputs", "/input/trigger/value"})
	result, err = call("wait_for", map[string]any{"path_json": string(pathJSON), "op": "near", "value": 0.8, "tol": 0.05, "timeout_ms": 1000})
	if err != nil {
		return report, err
	}
	var waitResult map[string]any
	_ = json.Unmarshal([]byte(toolText(result)), &waitResult)
	report.Add("wait_for on already-true condition -> met", boolean(waitResult["met"]), waitResult["met"])

	for _, operation := range []string{"walk_forward", "strafe"} {
		result, err = call(operation, map[string]any{"speed": 1.0, "duration_ms": 150})
		if err != nil {
			return report, err
		}
		value := toolText(result)
		report.Add(operation+" returns state (no error)", value != "" && !boolean(result["isError"]), fmt.Sprintf("len=%d", len(value)))
	}
	result, err = call("press", map[string]any{"hand": "right", "button": "a", "ms": 100})
	if err != nil {
		return report, err
	}
	valueText := toolText(result)
	report.Add("press returns state (no error)", valueText != "" && !boolean(result["isError"]), fmt.Sprintf("len=%d", len(valueText)))
	result, err = call("reset", map[string]any{})
	if err != nil {
		return report, err
	}
	state = stateFrom(result)
	z = resolve(state, "hmd", "head", "position", 2)
	report.Add("reset -> z~0", near(z, 0, 0.01), fmt.Sprintf("z=%v", z))
	return report, nil
}

func toolText(result map[string]any) string {
	content, _ := result["content"].([]any)
	for _, raw := range content {
		item, _ := raw.(map[string]any)
		if text(item["type"]) == "text" {
			return text(item["text"])
		}
	}
	return ""
}

func contentTypes(result map[string]any) []string {
	content, _ := result["content"].([]any)
	types := make([]string, 0, len(content))
	for _, raw := range content {
		item, _ := raw.(map[string]any)
		types = append(types, text(item["type"]))
	}
	return types
}

func containsContentType(result map[string]any, want string) bool {
	for _, contentType := range contentTypes(result) {
		if contentType == want {
			return true
		}
	}
	return false
}
