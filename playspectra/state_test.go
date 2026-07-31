package playspectra

import (
	"reflect"
	"testing"
)

func TestDefaultStateMatchesPythonReferenceFieldForField(t *testing.T) {
	state := DefaultModel().Snapshot(0).Raw()
	if state["protocol_version"] != float64(1) { // deliberate protocol precision absent from the Python snapshot
		t.Fatalf("protocol_version=%v", state["protocol_version"])
	}
	if state["sequence"] != float64(0) || !reflect.DeepEqual(state["clock"], map[string]any{"mode": "realtime"}) {
		t.Fatalf("envelope=%v", state)
	}
	for path, want := range map[string]any{
		"hmd/head/position":      []any{0.0, 1.6, 0.0},
		"hmd/head/orientation":   []any{0.0, 0.0, 0.0, 1.0},
		"left/grip/position":     []any{-0.2, 1.3, -0.5},
		"left/aim/position":      []any{-0.2, 1.3, -0.5},
		"right/grip/position":    []any{0.2, 1.3, -0.5},
		"right/aim/position":     []any{0.2, 1.3, -0.5},
		"right/grip/orientation": []any{0.0, 0.0, 0.0, 1.0},
		"right/aim/orientation":  []any{0.0, 0.0, 0.0, 1.0},
	} {
		parts := splitPath(path)
		if got := Resolve(state, parts); !reflect.DeepEqual(got, want) {
			t.Fatalf("%s=%#v want=%#v", path, got, want)
		}
	}
	for _, device := range []string{"hmd", "left", "right"} {
		if Resolve(state, []any{device, "connected"}) != true {
			t.Fatalf("%s disconnected", device)
		}
	}
	for _, posePath := range [][]any{{"hmd", "head"}, {"left", "grip"}, {"left", "aim"}, {"right", "grip"}, {"right", "aim"}} {
		flags, _ := Resolve(state, append(append([]any{}, posePath...), "relation_flags")).(map[string]any)
		if len(flags) != 4 || flags["position_valid"] != true || flags["orientation_valid"] != true || flags["position_tracked"] != true || flags["orientation_tracked"] != true {
			t.Fatalf("flags at %v=%v", posePath, flags)
		}
	}
	for hand, paths := range map[string][]string{"left": LeftInputPaths, "right": RightInputPaths} {
		inputs := Resolve(state, []any{hand, "inputs"}).(map[string]any)
		if len(inputs) != len(paths) {
			t.Fatalf("%s inputs=%d want=%d", hand, len(inputs), len(paths))
		}
		for _, path := range paths {
			value, present := inputs[path]
			if !present {
				t.Fatalf("%s missing %s", hand, path)
			}
			if hasNumericSuffix(path) {
				if _, ok := value.(float64); !ok || value != float64(0) {
					t.Fatalf("%s %s=%v (%T)", hand, path, value, value)
				}
			} else if _, ok := value.(bool); !ok || value != false {
				t.Fatalf("%s %s=%v (%T)", hand, path, value, value)
			}
		}
	}
}

func TestSeedHonorsDisconnectedStateAsIntentionalAccuracyImprovement(t *testing.T) {
	model := DefaultModel()
	model.Seed(map[string]any{"state": map[string]any{
		"left":  map[string]any{"connected": false},
		"right": map[string]any{"connected": true, "inputs": map[string]any{"/input/trigger/value": 0.75, "unknown": 9}},
	}})
	state := model.Snapshot(3).Raw()
	if !reflect.DeepEqual(state["left"], map[string]any{"connected": false}) {
		t.Fatalf("left=%v", state["left"])
	}
	if got := Resolve(state, []any{"right", "inputs", "/input/trigger/value"}); got != 0.75 {
		t.Fatalf("trigger=%v", got)
	}
	if got := Resolve(state, []any{"right", "inputs", "unknown"}); got != nil {
		t.Fatalf("unknown input was seeded: %v", got)
	}
}

func splitPath(path string) []any {
	parts := []any{}
	start := 0
	for index := 0; index <= len(path); index++ {
		if index == len(path) || path[index] == '/' {
			parts = append(parts, path[start:index])
			start = index + 1
		}
	}
	return parts
}
