package mcp

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/YmSaki/PlaySpectra/playspectra"
)

func TestInitializeAndToolList(t *testing.T) {
	h := NewHandler(func(context.Context) (*playspectra.Server, error) { return nil, nil })
	init, send := h.Handle(context.Background(), map[string]any{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": map[string]any{"protocolVersion": "2024-11-05"}})
	if !send || init["result"] == nil {
		t.Fatalf("initialize = %v", init)
	}
	list, send := h.Handle(context.Background(), map[string]any{"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
	if !send || !strings.Contains(playspectra.MarshalState(list), "run_scenario") {
		t.Fatalf("tools/list = %v", list)
	}
	ping, send := h.Handle(context.Background(), map[string]any{"jsonrpc": "2.0", "id": 3, "method": "ping"})
	if !send || ping["error"] != nil {
		t.Fatalf("ping = %v", ping)
	}
}

func TestNotificationsHaveNoResponse(t *testing.T) {
	h := NewHandler(nil)
	if response, send := h.Handle(context.Background(), map[string]any{"jsonrpc": "2.0", "method": "notifications/initialized"}); send || response != nil {
		t.Fatalf("notification response = %v %v", response, send)
	}
}

type argumentSpec struct {
	name         string
	typeName     string
	required     bool
	hasDefault   bool
	defaultValue any
}

func TestToolSchemasMatchPythonFastMCPCharacterization(t *testing.T) {
	// Captured from tools/playspectra_mcp.py with mcp 1.29.0. This checks
	// user-visible names, argument types, required fields, and defaults rather
	// than relying on the Go implementation's schema builder as its own oracle.
	expected := []struct {
		name       string
		arguments  []argumentSpec
		textOutput bool
	}{
		{"move_head", []argumentSpec{{"x", "number", true, false, nil}, {"y", "number", true, false, nil}, {"z", "number", true, false, nil}, {"duration_ms", "integer", false, true, 400}}, true},
		{"look", []argumentSpec{{"yaw_deg", "number", true, false, nil}, {"duration_ms", "integer", false, true, 400}}, true},
		{"walk_forward", []argumentSpec{{"speed", "number", false, true, 1.0}, {"duration_ms", "integer", false, true, 1000}, {"hand", "string", false, true, "left"}}, true},
		{"strafe", []argumentSpec{{"speed", "number", false, true, 1.0}, {"duration_ms", "integer", false, true, 1000}, {"hand", "string", false, true, "left"}}, true},
		{"press", []argumentSpec{{"hand", "string", false, true, "right"}, {"button", "string", false, true, "a"}, {"ms", "integer", false, true, 120}}, true},
		{"set_trigger", []argumentSpec{{"hand", "string", false, true, "right"}, {"value", "number", false, true, 1.0}, {"duration_ms", "integer", false, true, 200}}, true},
		{"move_controller", []argumentSpec{{"hand", "string", true, false, nil}, {"x", "number", true, false, nil}, {"y", "number", true, false, nil}, {"z", "number", true, false, nil}, {"duration_ms", "integer", false, true, 400}}, true},
		{"set_input", []argumentSpec{{"hand", "string", true, false, nil}, {"path", "string", true, false, nil}, {"value", "number", true, false, nil}, {"duration_ms", "integer", false, true, 0}}, true},
		{"reset", nil, true},
		{"get_state", nil, true},
		{"screenshot", []argumentSpec{{"eye", "string", false, true, "left"}}, false},
		{"wait_for", []argumentSpec{{"path_json", "string", true, false, nil}, {"op", "string", false, true, "near"}, {"value", "number", false, true, 0.0}, {"tol", "number", false, true, 0.01}, {"timeout_ms", "integer", false, true, 5000}}, true},
		{"run_scenario", []argumentSpec{{"scenario_json", "string", true, false, nil}}, true},
	}

	definitions := toolDefinitions()
	if len(definitions) != len(expected) {
		t.Fatalf("tool count = %d, want %d", len(definitions), len(expected))
	}
	for index, want := range expected {
		definition := definitions[index]
		if definition["name"] != want.name {
			t.Fatalf("tool %d name = %v, want %s", index, definition["name"], want.name)
		}
		if strings.TrimSpace(stringValue(definition["description"])) == "" {
			t.Fatalf("tool %s has no description", want.name)
		}
		schema := definition["inputSchema"].(map[string]any)
		if schema["type"] != "object" || schema["title"] != want.name+"Arguments" {
			t.Fatalf("tool %s input schema = %v", want.name, schema)
		}
		props := schema["properties"].(map[string]any)
		if len(props) != len(want.arguments) {
			t.Fatalf("tool %s properties = %v", want.name, props)
		}
		required := stringSet(schema["required"])
		for _, argument := range want.arguments {
			property, present := props[argument.name].(map[string]any)
			if !present || property["type"] != argument.typeName {
				t.Fatalf("tool %s argument %s = %v", want.name, argument.name, property)
			}
			if required[argument.name] != argument.required {
				t.Fatalf("tool %s argument %s required=%v, want %v", want.name, argument.name, required[argument.name], argument.required)
			}
			value, hasDefault := property["default"]
			if hasDefault != argument.hasDefault || (hasDefault && !numbersOrValuesEqual(value, argument.defaultValue)) {
				t.Fatalf("tool %s argument %s default=%v present=%v", want.name, argument.name, value, hasDefault)
			}
		}
		output, hasOutput := definition["outputSchema"].(map[string]any)
		if hasOutput != want.textOutput {
			t.Fatalf("tool %s output schema present=%v", want.name, hasOutput)
		}
		if hasOutput && output["title"] != want.name+"Output" {
			t.Fatalf("tool %s output schema = %v", want.name, output)
		}
	}
}

func stringSet(value any) map[string]bool {
	set := map[string]bool{}
	switch values := value.(type) {
	case []string:
		for _, item := range values {
			set[item] = true
		}
	case []any:
		for _, item := range values {
			if text, ok := item.(string); ok {
				set[text] = true
			}
		}
	}
	return set
}

func numbersOrValuesEqual(a, b any) bool {
	af, aok := floatValue(a)
	bf, bok := floatValue(b)
	if aok && bok {
		return af == bf
	}
	return reflect.DeepEqual(a, b)
}

type toolTransport struct {
	state    map[string]any
	requests []map[string]any
}

func newToolTransport() *toolTransport {
	return &toolTransport{state: playspectra.DefaultModel().Snapshot(0).Raw()}
}

func (f *toolTransport) Request(_ context.Context, request map[string]any) (map[string]any, error) {
	f.requests = append(f.requests, request)
	switch request["cmd"] {
	case "hello":
		return map[string]any{"ok": true, "role_granted": request["role"], "request_id": request["request_id"]}, nil
	case "get_state":
		return map[string]any{"ok": true, "state": f.state, "request_id": request["request_id"]}, nil
	case "reset":
		f.state = playspectra.DefaultModel().Snapshot(0).Raw()
		return map[string]any{"ok": true, "request_id": request["request_id"]}, nil
	default:
		return map[string]any{"ok": true, "request_id": request["request_id"]}, nil
	}
}

func (f *toolTransport) SendOnly(_ context.Context, request map[string]any) error {
	f.requests = append(f.requests, request)
	data, _ := json.Marshal(request["state"])
	return json.Unmarshal(data, &f.state)
}

type toolCapture struct{ path string }

func (c toolCapture) RequestLine(_ context.Context, _ map[string]any) (map[string]any, error) {
	return map[string]any{"ok": true, "path": c.path}, nil
}

func TestAllThirteenToolsReturnFastMCPCompatibleContent(t *testing.T) {
	png := []byte("\x89PNG\r\n\x1a\nfixture")
	path := t.TempDir() + "/capture.png"
	if err := os.WriteFile(path, png, 0o600); err != nil {
		t.Fatal(err)
	}
	transport := newToolTransport()
	factoryCalls := 0
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		factoryCalls++
		return playspectra.NewServer(transport, playspectra.WithSleeper(func(time.Duration) {}), playspectra.WithCapture(toolCapture{path: path})), nil
	})
	tests := []struct {
		name      string
		arguments map[string]any
		image     bool
	}{
		{"move_head", map[string]any{"x": 0.0, "y": 1.6, "z": -2.0, "duration_ms": 0}, false},
		{"look", map[string]any{"yaw_deg": 90.0, "duration_ms": 0}, false},
		{"walk_forward", map[string]any{"duration_ms": 0}, false},
		{"strafe", map[string]any{"duration_ms": 0}, false},
		{"press", map[string]any{"ms": 0}, false},
		{"set_trigger", map[string]any{"duration_ms": 0}, false},
		{"move_controller", map[string]any{"hand": "right", "x": 1.0, "y": 2.0, "z": 3.0, "duration_ms": 0}, false},
		{"set_input", map[string]any{"hand": "right", "path": "/button/a/click", "value": 1.0}, false},
		{"reset", map[string]any{}, false},
		{"get_state", map[string]any{}, false},
		{"screenshot", map[string]any{}, true},
		{"wait_for", map[string]any{"path_json": "[\"hmd\",\"head\",\"position\",2]", "value": 0.0, "timeout_ms": 0}, false},
		{"run_scenario", map[string]any{"scenario_json": "{\"steps\":[]}"}, false},
	}
	for index, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, send := h.Handle(context.Background(), map[string]any{
				"jsonrpc": "2.0", "id": index + 1, "method": "tools/call",
				"params": map[string]any{"name": test.name, "arguments": test.arguments},
			})
			if !send || response["error"] != nil {
				t.Fatalf("response = %v", response)
			}
			result := response["result"].(map[string]any)
			if result["isError"] != false {
				t.Fatalf("result = %v", result)
			}
			content := result["content"].([]any)
			if len(content) != 1 {
				t.Fatalf("content = %v", content)
			}
			item := content[0].(map[string]any)
			if test.image {
				if item["type"] != "image" || item["mimeType"] != "image/png" || item["data"] != base64.StdEncoding.EncodeToString(png) {
					t.Fatalf("image content = %v", item)
				}
				if _, present := result["structuredContent"]; present {
					t.Fatalf("screenshot unexpectedly has structured content: %v", result)
				}
				return
			}
			if item["type"] != "text" {
				t.Fatalf("text content = %v", item)
			}
			structured := result["structuredContent"].(map[string]any)
			if structured["result"] != item["text"] {
				t.Fatalf("structured content = %v, text = %v", structured, item["text"])
			}
		})
	}
	if factoryCalls != 1 {
		t.Fatalf("lazy factory calls = %d, want 1", factoryCalls)
	}
}

func TestUnknownToolAndInvalidArgumentsDoNotConnect(t *testing.T) {
	factoryCalls := 0
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		factoryCalls++
		return nil, nil
	})
	for id, params := range []map[string]any{
		{"name": "does_not_exist", "arguments": map[string]any{}},
		{"name": "set_input", "arguments": map[string]any{"path": "/button/a/click", "value": 1.0}},
	} {
		response, _ := h.Handle(context.Background(), map[string]any{"jsonrpc": "2.0", "id": id, "method": "tools/call", "params": params})
		result := response["result"].(map[string]any)
		if result["isError"] != true {
			t.Fatalf("result = %v", result)
		}
	}
	if factoryCalls != 0 {
		t.Fatalf("factory called %d times", factoryCalls)
	}
}

func TestCoreErrorIsReturnedAsToolError(t *testing.T) {
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		return playspectra.NewServer(newToolTransport(), playspectra.WithSleeper(func(time.Duration) {})), nil
	})
	response, _ := h.Handle(context.Background(), map[string]any{
		"jsonrpc": "2.0", "id": 1, "method": "tools/call",
		"params": map[string]any{"name": "set_input", "arguments": map[string]any{"hand": "middle", "path": "/button/a/click", "value": 1.0}},
	})
	result := response["result"].(map[string]any)
	content := result["content"].([]any)[0].(map[string]any)
	if result["isError"] != true || !strings.HasPrefix(stringValue(content["text"]), "Error executing tool set_input:") {
		t.Fatalf("result = %v", result)
	}
}

func TestScreenshotUnavailableIsSuccessfulTextLikePythonTool(t *testing.T) {
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		return playspectra.NewServer(newToolTransport()), nil
	})
	response, _ := h.Handle(context.Background(), map[string]any{
		"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": map[string]any{"name": "screenshot", "arguments": map[string]any{}},
	})
	result := response["result"].(map[string]any)
	content := result["content"].([]any)[0].(map[string]any)
	if result["isError"] != false || !strings.HasPrefix(stringValue(content["text"]), "screenshot failed: no capture channel") {
		t.Fatalf("result = %v", result)
	}
}

func TestServeReturnsStandardJSONRPCErrors(t *testing.T) {
	input := strings.Join([]string{
		"not-json",
		`{"jsonrpc":"2.0","id":1}`,
		`{"jsonrpc":"2.0","id":2,"method":"does/not/exist"}`,
		`{"jsonrpc":"2.0","method":"notifications/initialized"}`,
	}, "\n") + "\n"
	var output bytes.Buffer
	if err := NewHandler(nil).Serve(context.Background(), strings.NewReader(input), &output); err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(output.String()), "\n")
	if len(lines) != 3 {
		t.Fatalf("responses = %q", output.String())
	}
	wantCodes := []float64{-32700, -32600, -32601}
	for index, line := range lines {
		var response map[string]any
		if err := json.Unmarshal([]byte(line), &response); err != nil {
			t.Fatal(err)
		}
		errorObject := response["error"].(map[string]any)
		if errorObject["code"] != wantCodes[index] {
			t.Fatalf("response %d = %v", index, response)
		}
	}
}
