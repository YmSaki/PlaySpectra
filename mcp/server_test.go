package mcp

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
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

func TestToolDescriptionsMatchPythonFastMCPCharacterization(t *testing.T) {
	want := map[string]string{
		"move_head":       "Move the HMD viewpoint to a STAGE-space position in metres (x=right, y=up, z=-forward).\n    Interpolated. Returns the resulting device state as JSON.",
		"look":            "Turn the head by yaw_deg about world up (+Y); positive = left. Returns the device state.",
		"walk_forward":    "Hold the thumbstick forward (speed in [-1,1]) for duration, then release. Returns the state.",
		"strafe":          "Hold the thumbstick sideways (speed in [-1,1]; + is right) for duration, then release. The lateral\n    twin of walk_forward. Returns the state.",
		"press":           "Press and release a controller button (right: a/b, left: x/y). Returns the device state.",
		"set_trigger":     "Hold a controller trigger at value in [0,1] for duration. Returns the device state.",
		"move_controller": "Move a controller (hand = \"left\" | \"right\") grip+aim to a STAGE-space position in metres.\n    Interpolated. Returns the resulting device state.",
		"set_input":       "Set an arbitrary controller input path (e.g. '/input/squeeze/value', '/input/thumbstick/x') on\n    hand = \"left\"|\"right\". Use 1/0 for bool paths (/click, /touch). Returns the device state.",
		"reset":           "Reset the virtual devices to the builder-initial state. Returns the device state.",
		"get_state":       "Read the current virtual device state (HMD head pose + left/right controller grip/aim/inputs).",
		"screenshot":      "Capture and return the rendered eye image (PNG) the VR app is currently showing.\n    eye = \"left\" | \"right\" | \"dominant\". Needs the PlaySpectra layer (:52700) loaded in the app.",
		"wait_for":        "Auto-wait (Playwright-style) until a device-state field satisfies a condition, then return it.\n    Poll get_state until the field at path_json satisfies (op, value) or timeout_ms elapses -- use this\n    instead of a fixed sleep before reading state. path_json is a JSON array walking the state tree,\n    e.g. '[\"hmd\",\"head\",\"position\",2]' for head z, or '[\"right\",\"inputs\",\"/input/trigger/value\"]'.\n    op: near|eq|ne|gt|lt|true|false (true/false ignore value). Returns {\"met\": bool, \"state\": {...}}.",
		"run_scenario":    "Run a PlaySpectra JSON scenario (operate + assert + capture-assert steps) and return the\n    assertion summary {asserts, passed, failed, ok, failures}. The scenario is a self-checking test.",
	}
	definitions := toolDefinitions()
	if len(definitions) != len(want) {
		t.Fatalf("definitions=%d want=%d", len(definitions), len(want))
	}
	for _, definition := range definitions {
		name := stringValue(definition["name"])
		if got := stringValue(definition["description"]); got != want[name] {
			t.Fatalf("description %s:\n%q\nwant:\n%q", name, got, want[name])
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

func TestAdapterConnectionErrorIsReturnedAsToolError(t *testing.T) {
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		return nil, errors.New("dial adapter: connection refused")
	})
	response, _ := h.Handle(context.Background(), map[string]any{
		"jsonrpc": "2.0", "id": 1, "method": "tools/call",
		"params": map[string]any{"name": "get_state", "arguments": map[string]any{}},
	})
	result := response["result"].(map[string]any)
	content := result["content"].([]any)[0].(map[string]any)
	if result["isError"] != true || !strings.Contains(stringValue(content["text"]), "dial adapter: connection refused") {
		t.Fatalf("result=%v", result)
	}
}

func TestWaitForAcceptsJSONScalarAndBareKeyLikePython(t *testing.T) {
	// Both path_json spellings must reach the same field. The condition compares
	// against the field's real value (sequence is 0) instead of asking whether
	// it differs from 0: a "ne 0" check is satisfied by nil too, so it would
	// stay green even if path resolution returned nothing at all.
	for _, pathJSON := range []string{`"sequence"`, "sequence"} {
		for _, test := range []struct {
			value float64
			met   bool
		}{{0, true}, {1, false}} {
			t.Run(fmt.Sprintf("%s eq %g", pathJSON, test.value), func(t *testing.T) {
				h := NewHandler(func(context.Context) (*playspectra.Server, error) {
					return playspectra.NewServer(newToolTransport(), playspectra.WithSleeper(func(time.Duration) {})), nil
				})
				response, _ := h.Handle(context.Background(), map[string]any{
					"jsonrpc": "2.0", "id": 1, "method": "tools/call",
					"params": map[string]any{"name": "wait_for", "arguments": map[string]any{
						"path_json": pathJSON, "op": "eq", "value": test.value, "timeout_ms": 0,
					}},
				})
				result := response["result"].(map[string]any)
				body := stringValue(result["content"].([]any)[0].(map[string]any)["text"])
				if result["isError"] != false || !strings.Contains(body, fmt.Sprintf(`"met":%v`, test.met)) {
					t.Fatalf("result=%v", result)
				}
			})
		}
	}
}

func TestArgumentRejectionMessagesAreTheToolSurface(t *testing.T) {
	// These sentences are what an agent reads when a call is malformed, so they
	// are part of the tool surface rather than an internal detail. Each case
	// carries exactly one bad argument: validateArguments walks a map, so two
	// faults at once would make the reported one depend on iteration order.
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		t.Error("a rejected call still opened an adapter connection")
		return nil, errors.New("must not connect")
	})
	for _, test := range []struct {
		name      string
		tool      string
		arguments map[string]any
		want      string
	}{
		{"missing required argument", "move_head", map[string]any{"y": 1.6, "z": 0.0}, "Error executing tool move_head: x: field required"},
		{"string where a number belongs", "move_head", map[string]any{"x": "0", "y": 1.6, "z": 0.0}, "Error executing tool move_head: x: expected number"},
		{"number where a string belongs", "press", map[string]any{"hand": 1.0}, "Error executing tool press: hand: expected string"},
		{"fraction where an integer belongs", "move_head", map[string]any{"x": 0.0, "y": 1.6, "z": 0.0, "duration_ms": 400.5}, "Error executing tool move_head: duration_ms: expected integer"},
		{"unknown tool", "does_not_exist", map[string]any{}, "Unknown tool: does_not_exist"},
	} {
		t.Run(test.name, func(t *testing.T) {
			response, _ := h.Handle(context.Background(), map[string]any{
				"jsonrpc": "2.0", "id": 1, "method": "tools/call",
				"params": map[string]any{"name": test.tool, "arguments": test.arguments},
			})
			result := response["result"].(map[string]any)
			got := stringValue(result["content"].([]any)[0].(map[string]any)["text"])
			if result["isError"] != true || got != test.want {
				t.Fatalf("result=%v want text %q", result, test.want)
			}
		})
	}
}

func TestIntegerArgumentsArriveAsJSONFloatsAndAreAccepted(t *testing.T) {
	// Every other test hands the handler Go literals, so the integer-typed
	// arguments arrive as int. A real client sends JSON, and encoding/json
	// decodes 400 into float64 -- tightening the check to a `.(int)` assertion
	// would reject every genuine call while leaving the suite green. The request
	// is therefore decoded from wire text rather than written as a Go map.
	var request map[string]any
	if err := json.Unmarshal([]byte(`{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"move_head",
		"arguments":{"x":0,"y":1.6,"z":-2,"duration_ms":400}}}`), &request); err != nil {
		t.Fatal(err)
	}
	transport := newToolTransport()
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		return playspectra.NewServer(transport, playspectra.WithSleeper(func(time.Duration) {})), nil
	})
	response, _ := h.Handle(context.Background(), request)
	result := response["result"].(map[string]any)
	if result["isError"] != false {
		t.Fatalf("result = %v", result)
	}
	if got := playspectra.Resolve(transport.state, []any{"hmd", "head", "position", 2}); got != -2.0 {
		t.Fatalf("head z = %v, want the request to have been executed", got)
	}
}

func TestWaitForComparesBoolInputPathsNumericallyLikePython(t *testing.T) {
	// The MCP boundary only offers a number-typed "value", so the documented way
	// to wait on a bool path is 1/0. An unpressed /button/a/click must therefore
	// answer "ne 0" with met=false (otherwise waiting for a press succeeds before
	// anything is pressed) and "eq 0" with met=true.
	for _, test := range []struct {
		op  string
		met bool
	}{{"ne", false}, {"eq", true}} {
		t.Run(test.op, func(t *testing.T) {
			h := NewHandler(func(context.Context) (*playspectra.Server, error) {
				return playspectra.NewServer(newToolTransport(), playspectra.WithSleeper(func(time.Duration) {})), nil
			})
			response, _ := h.Handle(context.Background(), map[string]any{
				"jsonrpc": "2.0", "id": 1, "method": "tools/call",
				"params": map[string]any{"name": "wait_for", "arguments": map[string]any{
					"path_json": `["right","inputs","/button/a/click"]`, "op": test.op, "value": 0.0, "timeout_ms": 0,
				}},
			})
			result := response["result"].(map[string]any)
			body := stringValue(result["content"].([]any)[0].(map[string]any)["text"])
			if result["isError"] != false || !strings.Contains(body, fmt.Sprintf(`"met":%v`, test.met)) {
				t.Fatalf("op=%s result=%v", test.op, result)
			}
		})
	}
}

func TestWaitForOmittedArgumentsUseTheAdvertisedSchemaDefaults(t *testing.T) {
	// A default invocation sends only path_json, so op/value/tol/timeout_ms must
	// fall back to the values tools/list advertises (near, 0, 0.01, 5000) exactly
	// like FastMCP applied the Python signature defaults. Head z starts at 0, so
	// the condition already holds and the call must answer met=true on the first
	// poll; a dropped value default makes near() unsatisfiable and turns every
	// default invocation into a 5 s timeout. The server keeps its real sleeper so
	// that regression shows up as elapsed time rather than a busy loop.
	h := NewHandler(func(context.Context) (*playspectra.Server, error) {
		return playspectra.NewServer(newToolTransport()), nil
	})
	started := time.Now()
	response, _ := h.Handle(context.Background(), map[string]any{
		"jsonrpc": "2.0", "id": 1, "method": "tools/call",
		"params": map[string]any{"name": "wait_for", "arguments": map[string]any{
			"path_json": `["hmd","head","position",2]`,
		}},
	})
	elapsed := time.Since(started)
	result := response["result"].(map[string]any)
	body := stringValue(result["content"].([]any)[0].(map[string]any)["text"])
	if result["isError"] != false || !strings.Contains(body, `"met":true`) {
		t.Fatalf("result = %v", result)
	}
	if elapsed > time.Second {
		t.Fatalf("default wait_for took %v: it polled to the advertised timeout instead of matching immediately", elapsed)
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
