// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

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

func TestValidateNamesTheOffendingFieldForEveryRejection(t *testing.T) {
	// Validate is the gate every frame passes before it reaches the runtime, so
	// each rejection has to say which device, which path, and why -- an operator
	// reading only "validation_error" cannot tell a swapped [0,1]/[-1,1] range
	// from a bool written to a numeric path. The messages are asserted whole:
	// they are the contract, and a message that stops naming its field is the
	// regression this test exists to catch.
	for _, test := range []struct {
		name    string
		mutate  func(*State)
		wantErr string
	}{
		{"hmd pose", func(s *State) { s.HMD.Head.Position = []float64{0, 1.6} }, "validation_error: hmd.head pose dimensions"},
		{"controller grip pose", func(s *State) { s.Left.Grip.Orientation = []float64{0, 0, 1} }, "validation_error: left.grip pose dimensions"},
		{"controller aim pose", func(s *State) { s.Right.Aim.Position = nil }, "validation_error: right.aim pose dimensions"},
		{"missing input path", func(s *State) { delete(s.Right.Inputs, "/button/a/click") }, "validation_error: missing:/button/a/click hand:right"},
		{"bool on numeric path", func(s *State) { s.Left.Inputs["/input/trigger/value"] = true }, "validation_error: input:/input/trigger/value hand:left requires number"},
		{"number on bool path", func(s *State) { s.Right.Inputs["/button/a/click"] = 1.0 }, "validation_error: input:/button/a/click hand:right requires boolean"},
		{"value above one", func(s *State) { s.Left.Inputs["/input/trigger/value"] = 1.0001 }, "validation_error: input:/input/trigger/value out of range"},
		{"value below zero", func(s *State) { s.Left.Inputs["/input/squeeze/value"] = -0.0001 }, "validation_error: input:/input/squeeze/value out of range"},
		{"axis above one", func(s *State) { s.Right.Inputs["/input/thumbstick/x"] = 1.5 }, "validation_error: input:/input/thumbstick/x out of range"},
		{"axis below minus one", func(s *State) { s.Right.Inputs["/input/thumbstick/y"] = -1.5 }, "validation_error: input:/input/thumbstick/y out of range"},
	} {
		t.Run(test.name, func(t *testing.T) {
			state := DefaultModel().Snapshot(1)
			test.mutate(&state)
			err := state.Validate()
			if err == nil {
				t.Fatalf("%s was accepted", test.name)
			}
			if err.Error() != test.wantErr {
				t.Fatalf("error = %q, want %q", err, test.wantErr)
			}
		})
	}
}

func TestValidateAcceptsTheLegitimateBoundaries(t *testing.T) {
	// The counterweight to the rejection table: tightening a bound from <= to <
	// or reusing [0,1] for an axis would still reject everything above, so these
	// four are the inputs that tell a correct validator from an over-eager one.
	for _, test := range []struct {
		name  string
		state State
	}{
		{"initial adapter snapshot has sequence 0", DefaultModel().Snapshot(0)},
		{"trigger at full pull", inputState("left", "/input/trigger/value", 1.0)},
		{"thumbstick at negative full deflection", inputState("right", "/input/thumbstick/x", -1.0)},
		{"disconnected devices carry no pose or inputs", State{}},
	} {
		t.Run(test.name, func(t *testing.T) {
			if err := test.state.Validate(); err != nil {
				t.Fatalf("valid state rejected: %v", err)
			}
		})
	}
}

func inputState(hand, path string, value any) State {
	state := DefaultModel().Snapshot(1)
	if hand == "left" {
		state.Left.Inputs[path] = value
	} else {
		state.Right.Inputs[path] = value
	}
	return state
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
