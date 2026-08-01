package mcp

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"github.com/YmSaki/PlaySpectra/playspectra"
)

// requiredToolArguments holds one fixture value per required argument so every
// tool can be invoked with nothing but the arguments a client must supply. The
// table is checked against each tool's schema, so a new tool or a new required
// argument fails the test instead of quietly dropping out of the comparison.
var requiredToolArguments = map[string]map[string]any{
	"move_head":       {"x": 0.0, "y": 1.6, "z": -2.0},
	"look":            {"yaw_deg": 90.0},
	"walk_forward":    {},
	"strafe":          {},
	"press":           {},
	"set_trigger":     {},
	"move_controller": {"hand": "right", "x": 0.7, "y": 1.1, "z": -0.3},
	"set_input":       {"hand": "right", "path": "/input/squeeze/value", "value": 0.9},
	"reset":           {},
	"get_state":       {},
	"screenshot":      {},
	"wait_for":        {"path_json": `["hmd","head","position",2]`},
	"run_scenario":    {"scenario_json": `{"steps":[]}`},
}

// TestDefaultInvocationOfEveryToolMatchesExplicitDefaults is the comparison the
// Go port promised in docs/go-port-compatibility-plan.md: calling a tool with
// only its required arguments must be indistinguishable from calling it with
// every schema default spelled out. FastMCP applied the Python signature
// defaults for free, so a handler that forgets one is a silent port regression
// (wait_for's value default was exactly that). Nothing in the compared output
// is time-dependent, so the results are compared verbatim rather than
// normalised.
func TestDefaultInvocationOfEveryToolMatchesExplicitDefaults(t *testing.T) {
	definitions := toolDefinitions()
	if len(definitions) != len(requiredToolArguments) {
		t.Fatalf("registered tools = %d, fixture arguments = %d; update requiredToolArguments", len(definitions), len(requiredToolArguments))
	}
	directory := writeEyeFixtures(t)
	for _, definition := range definitions {
		name := stringValue(definition["name"])
		t.Run(name, func(t *testing.T) {
			required, present := requiredToolArguments[name]
			if !present {
				t.Fatalf("no fixture arguments for tool %s", name)
			}
			schema := definition["inputSchema"].(map[string]any)
			if got, want := keySet(required), stringSet(schema["required"]); !reflect.DeepEqual(got, want) {
				t.Fatalf("fixture supplies %v, schema requires %v", got, want)
			}
			explicit := map[string]any{}
			for key, value := range required {
				explicit[key] = value
			}
			for key, value := range schemaDefaults(schema) {
				explicit[key] = value
			}

			minimal, minimalCalls, minimalEyes := callToolOnFreshAdapter(t, directory, name, required)
			full, fullCalls, fullEyes := callToolOnFreshAdapter(t, directory, name, explicit)
			if boolValue(minimal["isError"]) {
				t.Fatalf("default invocation failed: %v", minimal)
			}
			if !reflect.DeepEqual(minimal, full) {
				t.Fatalf("default invocation differs from explicit defaults %v:\ndefault  = %v\nexplicit = %v", explicit, minimal, full)
			}
			// The adapter traffic is compared as well: a dropped duration_ms or
			// timeout_ms default changes how many frames or polls are sent
			// without necessarily changing the returned state.
			if !reflect.DeepEqual(minimalCalls, fullCalls) {
				t.Fatalf("adapter commands differ: default = %v, explicit = %v", minimalCalls, fullCalls)
			}
			if !reflect.DeepEqual(minimalEyes, fullEyes) {
				t.Fatalf("capture eyes differ: default = %v, explicit = %v", minimalEyes, fullEyes)
			}
		})
	}
}

func schemaDefaults(schema map[string]any) map[string]any {
	properties, _ := schema["properties"].(map[string]any)
	defaults := map[string]any{}
	for name, raw := range properties {
		property, _ := raw.(map[string]any)
		if value, present := property["default"]; present {
			defaults[name] = value
		}
	}
	return defaults
}

func keySet(values map[string]any) map[string]bool {
	set := map[string]bool{}
	for key := range values {
		set[key] = true
	}
	return set
}

// writeEyeFixtures gives each eye its own PNG so the screenshot comparison sees
// the eye argument rather than one shared image.
func writeEyeFixtures(t *testing.T) string {
	t.Helper()
	directory := t.TempDir()
	for _, eye := range []string{"left", "right", "dominant"} {
		if err := os.WriteFile(filepath.Join(directory, eye+".png"), []byte("\x89PNG\r\n\x1a\n"+eye), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return directory
}

func callToolOnFreshAdapter(t *testing.T, directory, name string, arguments map[string]any) (map[string]any, map[string]int, []string) {
	t.Helper()
	adapter := newDefaultsAdapter()
	handler := NewHandler(func(context.Context) (*playspectra.Server, error) {
		return playspectra.NewServer(adapter,
			playspectra.WithSleeper(func(time.Duration) {}),
			playspectra.WithCapture(defaultsCapture{adapter: adapter, directory: directory})), nil
	})
	response, send := handler.Handle(context.Background(), map[string]any{
		"jsonrpc": "2.0", "id": 1, "method": "tools/call",
		"params": map[string]any{"name": name, "arguments": arguments},
	})
	if !send || response["error"] != nil {
		t.Fatalf("%s(%v) response = %v", name, arguments, response)
	}
	result, ok := response["result"].(map[string]any)
	if !ok {
		t.Fatalf("%s(%v) result = %v", name, arguments, response)
	}
	return result, adapter.commands, adapter.eyes
}

// defaultsAdapter counts adapter commands instead of retaining them: a tool
// whose timeout default is lost polls until the deadline, and the recording
// must not grow with it.
type defaultsAdapter struct {
	state    map[string]any
	commands map[string]int
	eyes     []string
}

func newDefaultsAdapter() *defaultsAdapter {
	return &defaultsAdapter{state: playspectra.DefaultModel().Snapshot(0).Raw(), commands: map[string]int{}}
}

func (a *defaultsAdapter) Request(_ context.Context, request map[string]any) (map[string]any, error) {
	a.commands[stringValue(request["cmd"])]++
	switch request["cmd"] {
	case "hello":
		return map[string]any{"ok": true, "role_granted": request["role"], "request_id": request["request_id"]}, nil
	case "get_state":
		return map[string]any{"ok": true, "state": a.state, "request_id": request["request_id"]}, nil
	case "reset":
		a.state = playspectra.DefaultModel().Snapshot(0).Raw()
		return map[string]any{"ok": true, "request_id": request["request_id"]}, nil
	default:
		return map[string]any{"ok": true, "request_id": request["request_id"]}, nil
	}
}

func (a *defaultsAdapter) SendOnly(_ context.Context, request map[string]any) error {
	a.commands[stringValue(request["cmd"])]++
	data, _ := json.Marshal(request["state"])
	return json.Unmarshal(data, &a.state)
}

type defaultsCapture struct {
	adapter   *defaultsAdapter
	directory string
}

func (c defaultsCapture) RequestLine(_ context.Context, request map[string]any) (map[string]any, error) {
	eye := stringValue(request["eye"])
	c.adapter.eyes = append(c.adapter.eyes, eye)
	return map[string]any{"ok": true, "path": filepath.Join(c.directory, eye+".png")}, nil
}
