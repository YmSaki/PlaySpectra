// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Package mcp provides the dependency-free stdio MCP frontend for PlaySpectra.
package mcp

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/YmSaki/PlaySpectra/playspectra"
)

type Factory func(context.Context) (*playspectra.Server, error)

type Handler struct {
	factory Factory
	server  *playspectra.Server
}

func NewHandler(factory Factory) *Handler { return &Handler{factory: factory} }

func (h *Handler) getServer(ctx context.Context) (*playspectra.Server, error) {
	if h.server != nil {
		return h.server, nil
	}
	if h.factory == nil {
		return nil, fmt.Errorf("server factory is not configured")
	}
	server, err := h.factory(ctx)
	if err != nil {
		return nil, err
	}
	h.server = server
	return server, nil
}

func (h *Handler) Handle(ctx context.Context, request map[string]any) (map[string]any, bool) {
	id, hasID := request["id"]
	method := stringValue(request["method"])
	if !hasID || method == "notifications/initialized" {
		return nil, false
	}
	if request["jsonrpc"] != "2.0" || method == "" {
		return rpcError(id, -32600, "invalid request"), true
	}
	result, err := h.handleMethod(ctx, method, request)
	if err != nil {
		if strings.HasPrefix(err.Error(), "method not found:") {
			return rpcError(id, -32601, err.Error()), true
		}
		return rpcError(id, -32000, err.Error()), true
	}
	return map[string]any{"jsonrpc": "2.0", "id": id, "result": result}, true
}

func rpcError(id any, code int, message string) map[string]any {
	return map[string]any{"jsonrpc": "2.0", "id": id, "error": map[string]any{"code": code, "message": message}}
}

func (h *Handler) handleMethod(ctx context.Context, method string, request map[string]any) (map[string]any, error) {
	switch method {
	case "ping":
		return map[string]any{}, nil
	case "initialize":
		protocolVersion := "2024-11-05"
		if params, ok := request["params"].(map[string]any); ok {
			if value, ok := params["protocolVersion"].(string); ok && value != "" {
				protocolVersion = value
			}
		}
		return map[string]any{"protocolVersion": protocolVersion, "capabilities": map[string]any{"tools": map[string]any{}}, "serverInfo": map[string]any{"name": "playspectra", "version": "0.1.0"}}, nil
	case "tools/list":
		return map[string]any{"tools": toolDefinitions()}, nil
	case "tools/call":
		return h.callTool(ctx, request)
	default:
		return nil, fmt.Errorf("method not found: %s", method)
	}
}

func toolDefinitions() []map[string]any {
	definitions := []map[string]any{
		tool("move_head", "Move the HMD viewpoint to a STAGE-space position in metres (x=right, y=up, z=-forward).\n    Interpolated. Returns the resulting device state as JSON.", properties{
			"x": numberProperty("X"), "y": numberProperty("Y"), "z": numberProperty("Z"), "duration_ms": integerProperty("Duration Ms", 400),
		}, []string{"x", "y", "z"}, true),
		tool("look", "Turn the head by yaw_deg about world up (+Y); positive = left. Returns the device state.", properties{
			"yaw_deg": numberProperty("Yaw Deg"), "duration_ms": integerProperty("Duration Ms", 400),
		}, []string{"yaw_deg"}, true),
		tool("walk_forward", "Hold the thumbstick forward (speed in [-1,1]) for duration, then release. Returns the state.", properties{
			"speed": numberDefaultProperty("Speed", 1), "duration_ms": integerProperty("Duration Ms", 1000), "hand": stringProperty("Hand", "left"),
		}, nil, true),
		tool("strafe", "Hold the thumbstick sideways (speed in [-1,1]; + is right) for duration, then release. The lateral\n    twin of walk_forward. Returns the state.", properties{
			"speed": numberDefaultProperty("Speed", 1), "duration_ms": integerProperty("Duration Ms", 1000), "hand": stringProperty("Hand", "left"),
		}, nil, true),
		tool("press", "Press and release a controller button (right: a/b, left: x/y). Returns the device state.", properties{
			"hand": stringProperty("Hand", "right"), "button": stringProperty("Button", "a"), "ms": integerProperty("Ms", 120),
		}, nil, true),
		tool("set_trigger", "Hold a controller trigger at value in [0,1] for duration. Returns the device state.", properties{
			"hand": stringProperty("Hand", "right"), "value": numberDefaultProperty("Value", 1), "duration_ms": integerProperty("Duration Ms", 200),
		}, nil, true),
		tool("move_controller", "Move a controller (hand = \"left\" | \"right\") grip+aim to a STAGE-space position in metres.\n    Interpolated. Returns the resulting device state.", properties{
			"hand": requiredStringProperty("Hand"), "x": numberProperty("X"), "y": numberProperty("Y"), "z": numberProperty("Z"), "duration_ms": integerProperty("Duration Ms", 400),
		}, []string{"hand", "x", "y", "z"}, true),
		tool("set_input", "Set an arbitrary controller input path (e.g. '/input/squeeze/value', '/input/thumbstick/x') on\n    hand = \"left\"|\"right\". Use 1/0 for bool paths (/click, /touch). Returns the device state.", properties{
			"hand": requiredStringProperty("Hand"), "path": requiredStringProperty("Path"), "value": numberProperty("Value"), "duration_ms": integerProperty("Duration Ms", 0),
		}, []string{"hand", "path", "value"}, true),
		tool("reset", "Reset the virtual devices to the builder-initial state. Returns the device state.", properties{}, nil, true),
		tool("get_state", "Read the current virtual device state (HMD head pose + left/right controller grip/aim/inputs).", properties{}, nil, true),
		tool("screenshot", "Capture and return the rendered eye image (PNG) the VR app is currently showing.\n    eye = \"left\" | \"right\" | \"dominant\". Needs the PlaySpectra layer (:52700) loaded in the app.", properties{
			"eye": stringProperty("Eye", "left"),
		}, nil, false),
		tool("wait_for", "Auto-wait (Playwright-style) until a device-state field satisfies a condition, then return it.\n    Poll get_state until the field at path_json satisfies (op, value) or timeout_ms elapses -- use this\n    instead of a fixed sleep before reading state. path_json is a JSON array walking the state tree,\n    e.g. '[\"hmd\",\"head\",\"position\",2]' for head z, or '[\"right\",\"inputs\",\"/input/trigger/value\"]'.\n    op: near|eq|ne|gt|lt|true|false (true/false ignore value). Returns {\"met\": bool, \"state\": {...}}.", properties{
			"path_json": requiredStringProperty("Path Json"), "op": stringProperty("Op", "near"), "value": numberDefaultProperty("Value", 0), "tol": numberDefaultProperty("Tol", 0.01), "timeout_ms": integerProperty("Timeout Ms", 5000),
		}, []string{"path_json"}, true),
		tool("run_scenario", "Run a PlaySpectra JSON scenario (operate + assert + capture-assert steps) and return the\n    assertion summary {asserts, passed, failed, ok, failures}. The scenario is a self-checking test.", properties{
			"scenario_json": requiredStringProperty("Scenario Json"),
		}, []string{"scenario_json"}, true),
	}
	return definitions
}

type properties map[string]any

func tool(name, description string, props properties, required []string, stringOutput bool) map[string]any {
	input := map[string]any{"properties": map[string]any(props), "title": name + "Arguments", "type": "object"}
	if len(required) > 0 {
		input["required"] = required
	}
	definition := map[string]any{"name": name, "description": description, "inputSchema": input}
	if stringOutput {
		definition["outputSchema"] = map[string]any{
			"properties": map[string]any{"result": map[string]any{"title": "Result", "type": "string"}},
			"required":   []string{"result"}, "title": name + "Output", "type": "object",
		}
	}
	return definition
}

func numberProperty(title string) map[string]any {
	return map[string]any{"title": title, "type": "number"}
}
func numberDefaultProperty(title string, value float64) map[string]any {
	return map[string]any{"default": value, "title": title, "type": "number"}
}
func integerProperty(title string, value int) map[string]any {
	return map[string]any{"default": value, "title": title, "type": "integer"}
}
func stringProperty(title, value string) map[string]any {
	return map[string]any{"default": value, "title": title, "type": "string"}
}
func requiredStringProperty(title string) map[string]any {
	return map[string]any{"title": title, "type": "string"}
}

func (h *Handler) callTool(ctx context.Context, request map[string]any) (map[string]any, error) {
	params, _ := request["params"].(map[string]any)
	name := stringValue(params["name"])
	args, _ := params["arguments"].(map[string]any)
	if args == nil {
		args = map[string]any{}
	}
	definition := findTool(name)
	if definition == nil {
		return errorResult(fmt.Errorf("Unknown tool: %s", name)), nil
	}
	if validationErr := validateArguments(definition, args); validationErr != nil {
		return errorResult(fmt.Errorf("Error executing tool %s: %w", name, validationErr)), nil
	}
	server, err := h.getServer(ctx)
	if err != nil {
		return errorResult(fmt.Errorf("Error executing tool %s: %w", name, err)), nil
	}
	textResult := func(value any) (map[string]any, error) {
		data, _ := json.Marshal(value)
		return successResult(string(data)), nil
	}
	stateResult := func() (map[string]any, error) {
		state, err := server.GetState(ctx)
		if err != nil {
			return errorResult(err), nil
		}
		return textResult(state)
	}
	num := func(key string, fallback float64) float64 {
		if value, ok := args[key]; ok {
			if v, ok := floatValue(value); ok {
				return v
			}
		}
		return fallback
	}
	integer := func(key string, fallback int) int { return int(num(key, float64(fallback))) }
	str := func(key, fallback string) string {
		if value, ok := args[key].(string); ok {
			return value
		}
		return fallback
	}
	to := func() map[string]any {
		return map[string]any{"position": []any{num("x", 0), num("y", 1.6), num("z", 0)}}
	}
	var value any
	switch name {
	case "move_head":
		err = server.MoveHead(ctx, to(), integer("duration_ms", 400))
		if err == nil {
			return stateResult()
		}
	case "look":
		err = server.Look(ctx, num("yaw_deg", 0), integer("duration_ms", 400))
		if err == nil {
			return stateResult()
		}
	case "walk_forward":
		err = server.WalkForward(ctx, num("speed", 1), integer("duration_ms", 1000), str("hand", "left"))
		if err == nil {
			return stateResult()
		}
	case "strafe":
		err = server.Strafe(ctx, num("speed", 1), integer("duration_ms", 1000), str("hand", "left"))
		if err == nil {
			return stateResult()
		}
	case "press":
		err = server.Press(ctx, str("hand", "right"), str("button", "a"), integer("ms", 120))
		if err == nil {
			return stateResult()
		}
	case "set_trigger":
		err = server.SetTrigger(ctx, str("hand", "right"), num("value", 1), integer("duration_ms", 200))
		if err == nil {
			return stateResult()
		}
	case "move_controller":
		err = server.MoveController(ctx, str("hand", "right"), to(), integer("duration_ms", 400))
		if err == nil {
			return stateResult()
		}
	case "set_input":
		value = args["value"]
		err = server.SetInput(ctx, str("hand", "right"), str("path", ""), value, integer("duration_ms", 0))
		if err == nil {
			return stateResult()
		}
	case "reset":
		err = server.Reset(ctx)
		if err == nil {
			return stateResult()
		}
	case "get_state":
		return stateResult()
	case "screenshot":
		result, screenshotErr := server.Screenshot(ctx, str("eye", "left"), 8000)
		if screenshotErr != nil {
			return errorResult(screenshotErr), nil
		}
		if !boolValue(result["ok"]) {
			return plainTextResult("screenshot failed: "+stringValue(result["error"]), false), nil
		}
		path := stringValue(result["path"])
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			return errorResult(readErr), nil
		}
		return map[string]any{"content": []any{map[string]any{"type": "image", "data": base64.StdEncoding.EncodeToString(data), "mimeType": "image/png"}}, "isError": false}, nil
	case "wait_for":
		rawPath := str("path_json", "[]")
		var decodedPath any
		if unmarshalErr := json.Unmarshal([]byte(rawPath), &decodedPath); unmarshalErr != nil {
			decodedPath = rawPath
		}
		path, isArray := decodedPath.([]any)
		if !isArray {
			path = []any{decodedPath}
		}
		met, waitErr := server.WaitFor(ctx, path, str("op", "near"), num("value", 0), num("tol", 0.01), integer("timeout_ms", 5000), 50, "")
		if waitErr != nil {
			return errorResult(waitErr), nil
		}
		state, stateErr := server.GetState(ctx)
		if stateErr != nil {
			return errorResult(stateErr), nil
		}
		return textResult(map[string]any{"met": met, "state": state})
	case "run_scenario":
		server.ResetAssertions()
		summary, scenarioErr := server.RunScenarioJSON(ctx, []byte(str("scenario_json", "{}")))
		if scenarioErr != nil {
			return errorResult(scenarioErr), nil
		}
		return textResult(summary)
	default:
		return errorResult(fmt.Errorf("Unknown tool: %s", name)), nil
	}
	return errorResult(fmt.Errorf("Error executing tool %s: %w", name, err)), nil
}

func successResult(text string) map[string]any {
	result := plainTextResult(text, false)
	result["structuredContent"] = map[string]any{"result": text}
	return result
}
func errorResult(err error) map[string]any {
	return plainTextResult(err.Error(), true)
}
func plainTextResult(text string, isError bool) map[string]any {
	return map[string]any{"content": []any{map[string]any{"type": "text", "text": text}}, "isError": isError}
}

func findTool(name string) map[string]any {
	for _, definition := range toolDefinitions() {
		if definition["name"] == name {
			return definition
		}
	}
	return nil
}

func validateArguments(definition map[string]any, arguments map[string]any) error {
	schema, _ := definition["inputSchema"].(map[string]any)
	required, _ := schema["required"].([]string)
	for _, name := range required {
		if _, present := arguments[name]; !present {
			return fmt.Errorf("%s: field required", name)
		}
	}
	props, _ := schema["properties"].(map[string]any)
	for name, value := range arguments {
		property, declared := props[name].(map[string]any)
		if !declared {
			continue
		}
		switch property["type"] {
		case "string":
			if _, ok := value.(string); !ok {
				return fmt.Errorf("%s: expected string", name)
			}
		case "number":
			if _, ok := floatValue(value); !ok {
				return fmt.Errorf("%s: expected number", name)
			}
		case "integer":
			number, ok := floatValue(value)
			if !ok || number != float64(int64(number)) {
				return fmt.Errorf("%s: expected integer", name)
			}
		}
	}
	return nil
}
func floatValue(value any) (float64, bool) {
	switch v := value.(type) {
	case float64:
		return v, true
	case float32:
		return float64(v), true
	case int:
		return float64(v), true
	case int64:
		return float64(v), true
	case json.Number:
		f, e := v.Float64()
		return f, e == nil
	default:
		return 0, false
	}
}
func stringValue(value any) string { text, _ := value.(string); return text }
func boolValue(value any) bool     { flag, _ := value.(bool); return flag }

func (h *Handler) Serve(ctx context.Context, in io.Reader, out io.Writer) error {
	scanner := bufio.NewScanner(in)
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	writer := bufio.NewWriter(out)
	defer writer.Flush()
	writeResponse := func(response map[string]any) error {
		data, err := json.Marshal(response)
		if err != nil {
			return err
		}
		if _, err := writer.Write(append(data, '\n')); err != nil {
			return err
		}
		return writer.Flush()
	}
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		var request map[string]any
		if err := json.Unmarshal([]byte(line), &request); err != nil {
			if err := writeResponse(rpcError(nil, -32700, "parse error")); err != nil {
				return err
			}
			continue
		}
		response, send := h.Handle(ctx, request)
		if !send {
			continue
		}
		if err := writeResponse(response); err != nil {
			return err
		}
	}
	return scanner.Err()
}
