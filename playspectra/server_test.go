// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package playspectra

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"testing"
	"time"
)

type fakeTransport struct {
	state    map[string]any
	requests []map[string]any
}

func sentStates(t *testing.T, transport *fakeTransport) []map[string]any {
	t.Helper()
	states := []map[string]any{}
	for _, request := range transport.requests {
		if request["cmd"] != "set_state" {
			continue
		}
		data, err := json.Marshal(request["state"])
		if err != nil {
			t.Fatal(err)
		}
		var state map[string]any
		if err := json.Unmarshal(data, &state); err != nil {
			t.Fatal(err)
		}
		states = append(states, state)
	}
	return states
}

func newFakeTransport() *fakeTransport {
	return &fakeTransport{state: DefaultModel().Snapshot(0).Raw()}
}

func (f *fakeTransport) Request(_ context.Context, request map[string]any) (map[string]any, error) {
	f.requests = append(f.requests, request)
	switch request["cmd"] {
	case "hello":
		return map[string]any{"ok": true, "role_granted": request["role"], "request_id": request["request_id"]}, nil
	case "get_state":
		return map[string]any{"ok": true, "state": f.state, "request_id": request["request_id"]}, nil
	case "reset":
		f.state = DefaultModel().Snapshot(0).Raw()
		return map[string]any{"ok": true, "request_id": request["request_id"]}, nil
	default:
		return map[string]any{"ok": true, "request_id": request["request_id"]}, nil
	}
}

func (f *fakeTransport) SendOnly(_ context.Context, request map[string]any) error {
	f.requests = append(f.requests, request)
	if state, ok := request["state"].(State); ok {
		f.state = state.Raw()
		return nil
	}
	data, _ := json.Marshal(request["state"])
	var state map[string]any
	_ = json.Unmarshal(data, &state)
	f.state = state
	return nil
}

func TestServerOperationsAndFullSnapshots(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
	if err := server.Hello(context.Background(), "writer"); err != nil {
		t.Fatal(err)
	}
	if err := server.MoveHead(context.Background(), map[string]any{"position": []any{0, 1.6, -1.5}}, 20); err != nil {
		t.Fatal(err)
	}
	state, err := server.GetState(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if got := Resolve(state, []any{"hmd", "head", "position", 2}); got.(float64) != -1.5 {
		t.Fatalf("head z = %v", got)
	}
	if err := server.WalkForward(context.Background(), 1, 20, "left"); err != nil {
		t.Fatal(err)
	}
	state, _ = server.GetState(context.Background())
	if got := Resolve(state, []any{"left", "inputs", "/input/thumbstick/y"}); got.(float64) != 0 {
		t.Fatalf("stick was not released: %v", got)
	}
	if err := server.SetInput(context.Background(), "right", "/button/a/click", 1, 0); err != nil {
		t.Fatal(err)
	}
	state, _ = server.GetState(context.Background())
	if got := Resolve(state, []any{"right", "inputs", "/button/a/click"}); got != true {
		t.Fatalf("button = %v", got)
	}
	if len(transport.requests) == 0 {
		t.Fatal("no requests recorded")
	}
}

func TestHelloSeedsNonDefaultAdapterState(t *testing.T) {
	transport := newFakeTransport()
	transport.state["sequence"] = float64(17)
	transport.state["hmd"].(map[string]any)["head"].(map[string]any)["position"] = []any{1.0, 2.0, 3.0}
	transport.state["right"].(map[string]any)["grip"].(map[string]any)["position"] = []any{4.0, 5.0, 6.0}
	transport.state["right"].(map[string]any)["inputs"].(map[string]any)["/input/trigger/value"] = 0.75

	server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
	if err := server.Hello(context.Background(), "writer"); err != nil {
		t.Fatal(err)
	}
	if got := server.Model.HeadPos; got[0] != 1 || got[1] != 2 || got[2] != 3 {
		t.Fatalf("head position was not seeded: %v", got)
	}
	if got := server.Model.Right.Grip.Position; got[0] != 4 || got[1] != 5 || got[2] != 6 {
		t.Fatalf("right grip position was not seeded: %v", got)
	}
	if got := server.Model.Right.Inputs["/input/trigger/value"]; got != 0.75 {
		t.Fatalf("right trigger was not seeded: %v", got)
	}
	if server.Seq != 17 {
		t.Fatalf("sequence = %d, want 17", server.Seq)
	}
	if len(transport.requests) != 2 || transport.requests[0]["cmd"] != "hello" || transport.requests[0]["protocol_version"] != 1 || transport.requests[0]["role"] != "writer" || transport.requests[1]["cmd"] != "get_state" {
		t.Fatalf("hello request sequence=%v", transport.requests)
	}
}

func TestHelloRejectsAdapterRoleFailure(t *testing.T) {
	// A refused hello is almost always the writer slot being held by another
	// client, and the adapter's own sentence is the only diagnosis the operator
	// gets, so it has to survive the wrap instead of collapsing into a generic
	// "hello failed".
	for _, test := range []struct {
		name     string
		response map[string]any
		wantErr  string
	}{
		{"adapter message", map[string]any{"ok": false, "error": "writer already connected"}, "hello failed: writer already connected"},
		{"rejection without a message", map[string]any{"ok": false}, "hello failed: map[ok:false]"},
		{"no response at all", nil, "hello failed: empty response"},
	} {
		t.Run(test.name, func(t *testing.T) {
			err := NewServer(fixedReplyTransport{response: test.response}).Hello(context.Background(), "writer")
			if err == nil {
				t.Fatal("failed hello was accepted")
			}
			if err.Error() != test.wantErr {
				t.Fatalf("error = %q, want %q", err, test.wantErr)
			}
		})
	}
}

type grantedRoleTransport struct {
	*fakeTransport
	granted string
}

func (g grantedRoleTransport) Request(ctx context.Context, request map[string]any) (map[string]any, error) {
	response, err := g.fakeTransport.Request(ctx, request)
	if request["cmd"] == "hello" && response != nil {
		response["role_granted"] = g.granted
	}
	return response, err
}

func TestHelloIgnoresGrantedRoleWhileReplayerEnforcesIt(t *testing.T) {
	// Characterization of a deliberate asymmetry, not a recommendation: the
	// Python Server.hello looked only at "ok", while its Recorder and Replayer
	// also compared role_granted. Keeping that split pinned means making the two
	// symmetric later is a conscious compatibility decision rather than drift.
	transport := grantedRoleTransport{fakeTransport: newFakeTransport(), granted: "observer"}
	if err := NewServer(transport, WithSleeper(func(time.Duration) {})).Hello(context.Background(), "writer"); err != nil {
		t.Fatalf("Server.Hello now enforces role_granted: %v", err)
	}
	if err := NewReplayer(transport).Hello(context.Background()); err == nil {
		t.Fatal("Replayer accepted an observer grant for a writer hello")
	}
}

func TestDisconnectedDeviceSnapshotContainsOnlyConnectedFlag(t *testing.T) {
	model := DefaultModel()
	model.Right.Connected = false
	state := model.Snapshot(1).Raw()
	right, ok := state["right"].(map[string]any)
	if !ok {
		t.Fatalf("right state = %T", state["right"])
	}
	if len(right) != 1 || right["connected"] != false {
		t.Fatalf("disconnected right state = %v, want only connected:false", right)
	}
}

func TestResetDoesNotRewindWriterSequence(t *testing.T) {
	transport := newFakeTransport()
	transport.state["sequence"] = float64(40)
	server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
	if err := server.Hello(context.Background(), "writer"); err != nil {
		t.Fatal(err)
	}
	if err := server.MoveHead(context.Background(), map[string]any{}, 0); err != nil {
		t.Fatal(err)
	}
	if server.Seq != 41 {
		t.Fatalf("sequence before reset = %d, want 41", server.Seq)
	}
	if err := server.Reset(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := server.MoveHead(context.Background(), map[string]any{}, 0); err != nil {
		t.Fatal(err)
	}
	if server.Seq != 42 {
		t.Fatalf("sequence after reset = %d, want 42", server.Seq)
	}
	if got := transport.state["sequence"]; got != float64(42) {
		t.Fatalf("adapter sequence after reset = %v, want 42", got)
	}
}

func TestInterpolationFrameCountUsesPythonTiesToEvenRounding(t *testing.T) {
	for _, test := range []struct {
		durationMS int
		wantFrames int
	}{
		{0, 1}, {100, 1}, {240, 2}, {250, 2}, {260, 3},
	} {
		t.Run(fmt.Sprintf("%dms", test.durationMS), func(t *testing.T) {
			transport := newFakeTransport()
			server := NewServer(transport, WithRate(10), WithSleeper(func(_ time.Duration) {}))
			if err := server.MoveHead(context.Background(), map[string]any{}, test.durationMS); err != nil {
				t.Fatal(err)
			}
			if frames := len(sentStates(t, transport)); frames != test.wantFrames {
				t.Fatalf("set_state frames = %d, want %d", frames, test.wantFrames)
			}
		})
	}
}

func TestPythonScenarioOperationDefaultsAndFrameCounts(t *testing.T) {
	tests := []struct {
		name       string
		step       map[string]any
		wantFrames int
		firstPath  []any
		firstValue any
		lastPath   []any
		lastValue  any
	}{
		{
			name: "move_head defaults to 500ms", step: map[string]any{"cmd": "move_head", "to": map[string]any{"position": []any{0, 1.6, -2}}}, wantFrames: 30,
			lastPath: []any{"hmd", "head", "position", 2}, lastValue: -2.0,
		},
		{
			name: "look defaults to 500ms", step: map[string]any{"cmd": "look", "yaw_deg": 90.0}, wantFrames: 30,
			lastPath: []any{"hmd", "head", "orientation", 1}, lastValue: math.Sqrt(0.5),
		},
		{
			name: "walk defaults to left for 1000ms plus release", step: map[string]any{"cmd": "walk_forward", "speed": 0.5}, wantFrames: 61,
			firstPath: []any{"left", "inputs", "/input/thumbstick/y"}, firstValue: 0.5,
			lastPath: []any{"left", "inputs", "/input/thumbstick/y"}, lastValue: 0.0,
		},
		{
			name: "strafe defaults to left for 1000ms plus release", step: map[string]any{"cmd": "strafe", "speed": -0.5}, wantFrames: 61,
			firstPath: []any{"left", "inputs", "/input/thumbstick/x"}, firstValue: -0.5,
			lastPath: []any{"left", "inputs", "/input/thumbstick/x"}, lastValue: 0.0,
		},
		{
			name: "trigger defaults to right for 200ms", step: map[string]any{"cmd": "trigger"}, wantFrames: 12,
			lastPath: []any{"right", "inputs", "/input/trigger/value"}, lastValue: 1.0,
		},
		{
			name: "move_controller defaults to right for 400ms", step: map[string]any{"cmd": "move_controller", "to": map[string]any{"position": []any{1, 2, 3}}}, wantFrames: 24,
			lastPath: []any{"right", "grip", "position", 2}, lastValue: 3.0,
		},
		{
			name: "set_input defaults to right and instant", step: map[string]any{"cmd": "set_input", "path": "/button/a/click", "value": 1.0}, wantFrames: 1,
			lastPath: []any{"right", "inputs", "/button/a/click"}, lastValue: true,
		},
		{
			name: "press defaults to right a and two frames", step: map[string]any{"cmd": "press"}, wantFrames: 2,
			firstPath: []any{"right", "inputs", "/button/a/click"}, firstValue: true,
			lastPath: []any{"right", "inputs", "/button/a/click"}, lastValue: false,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			transport := newFakeTransport()
			server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
			if _, err := server.RunStep(context.Background(), test.step); err != nil {
				t.Fatal(err)
			}
			states := sentStates(t, transport)
			if len(states) != test.wantFrames {
				t.Fatalf("frames = %d, want %d", len(states), test.wantFrames)
			}
			if states[0]["protocol_version"] != float64(1) {
				t.Fatalf("protocol_version = %v", states[0]["protocol_version"])
			}
			if test.firstPath != nil {
				if got := Resolve(states[0], test.firstPath); !valuesNear(got, test.firstValue) {
					t.Fatalf("first frame %v = %v, want %v", test.firstPath, got, test.firstValue)
				}
			}
			if got := Resolve(states[len(states)-1], test.lastPath); !valuesNear(got, test.lastValue) {
				t.Fatalf("last frame %v = %v, want %v", test.lastPath, got, test.lastValue)
			}
		})
	}
}

func TestMoveHeadEmitsEveryInterpolatedFullFrameWithMonotonicSequence(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithRate(10), WithSleeper(func(_ time.Duration) {}))
	if err := server.MoveHead(context.Background(), map[string]any{"position": []any{0, 1.6, -2}}, 400); err != nil {
		t.Fatal(err)
	}
	states := sentStates(t, transport)
	wantZ := []float64{-0.5, -1, -1.5, -2}
	if len(states) != len(wantZ) {
		t.Fatalf("frames = %d, want %d", len(states), len(wantZ))
	}
	for index, state := range states {
		if err := validateRawState(state); err != nil {
			t.Fatalf("frame %d is not a complete snapshot: %v", index, err)
		}
		if state["sequence"] != float64(index+1) {
			t.Fatalf("frame %d sequence = %v", index, state["sequence"])
		}
		if got := Resolve(state, []any{"hmd", "head", "position", 2}); got != wantZ[index] {
			t.Fatalf("frame %d head z = %v, want %v", index, got, wantZ[index])
		}
	}
}

func TestPoseAndInputOperationsMatchPythonFrameSemantics(t *testing.T) {
	ctx := context.Background()
	t.Run("move head position and orientation slerp", func(t *testing.T) {
		transport := newFakeTransport()
		server := NewServer(transport, WithRate(4), WithSleeper(func(time.Duration) {}))
		target := []any{0.0, math.Sqrt(0.5), 0.0, math.Sqrt(0.5)}
		if err := server.MoveHead(ctx, map[string]any{"position": []any{0.0, 1.6, -2.0}, "orientation": target}, 1000); err != nil {
			t.Fatal(err)
		}
		states := sentStates(t, transport)
		for index, state := range states {
			fraction := float64(index+1) / 4
			if got := Resolve(state, []any{"hmd", "head", "position", 2}); !valuesNear(got, -2*fraction) {
				t.Fatalf("frame %d z=%v", index, got)
			}
			wantY, wantW := math.Sin((math.Pi/2*fraction)/2), math.Cos((math.Pi/2*fraction)/2)
			if got := Resolve(state, []any{"hmd", "head", "orientation", 1}); !valuesNear(got, wantY) {
				t.Fatalf("frame %d qy=%v want=%v", index, got, wantY)
			}
			if got := Resolve(state, []any{"hmd", "head", "orientation", 3}); !valuesNear(got, wantW) {
				t.Fatalf("frame %d qw=%v want=%v", index, got, wantW)
			}
		}
	})

	for _, test := range []struct {
		name  string
		apply func(*Server) error
		path  []any
		want  []float64
	}{
		{"walk clamps high and releases", func(s *Server) error { return s.WalkForward(ctx, 2, 500, "left") }, []any{"left", "inputs", "/input/thumbstick/y"}, []float64{1, 1, 0}},
		{"strafe clamps low and releases", func(s *Server) error { return s.Strafe(ctx, -2, 500, "right") }, []any{"right", "inputs", "/input/thumbstick/x"}, []float64{-1, -1, 0}},
		{"trigger clamps high and stays held", func(s *Server) error { return s.SetTrigger(ctx, "right", 2, 500) }, []any{"right", "inputs", "/input/trigger/value"}, []float64{1, 1}},
		{"trigger clamps low", func(s *Server) error { return s.SetTrigger(ctx, "left", -1, 500) }, []any{"left", "inputs", "/input/trigger/value"}, []float64{0, 0}},
	} {
		t.Run(test.name, func(t *testing.T) {
			transport := newFakeTransport()
			server := NewServer(transport, WithRate(4), WithSleeper(func(time.Duration) {}))
			if err := test.apply(server); err != nil {
				t.Fatal(err)
			}
			states := sentStates(t, transport)
			if len(states) != len(test.want) {
				t.Fatalf("frames=%d want=%d", len(states), len(test.want))
			}
			for index, want := range test.want {
				if got := Resolve(states[index], test.path); !valuesNear(got, want) {
					t.Fatalf("frame %d value=%v want=%v", index, got, want)
				}
			}
		})
	}
}

func TestMoveControllerKeepsGripAndAimSynchronized(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithRate(2), WithSleeper(func(time.Duration) {}))
	targetOrientation := []any{0.0, math.Sqrt(0.5), 0.0, math.Sqrt(0.5)}
	if err := server.MoveController(context.Background(), "right", map[string]any{
		"position": []any{0.6, 1.5, -0.1}, "orientation": targetOrientation,
	}, 1000); err != nil {
		t.Fatal(err)
	}
	states := sentStates(t, transport)
	if len(states) != 2 {
		t.Fatalf("frames=%d", len(states))
	}
	for index, state := range states {
		for component := 0; component < 3; component++ {
			grip := Resolve(state, []any{"right", "grip", "position", component})
			aim := Resolve(state, []any{"right", "aim", "position", component})
			if !valuesNear(grip, aim) {
				t.Fatalf("frame %d component %d grip=%v aim=%v", index, component, grip, aim)
			}
		}
		for component := 0; component < 4; component++ {
			grip := Resolve(state, []any{"right", "grip", "orientation", component})
			aim := Resolve(state, []any{"right", "aim", "orientation", component})
			if !valuesNear(grip, aim) {
				t.Fatalf("frame %d orientation %d grip=%v aim=%v", index, component, grip, aim)
			}
		}
	}
}

func TestLookHandlesZeroNegativeAndLargeAngles(t *testing.T) {
	for _, degrees := range []float64{0, -90, 450} {
		t.Run(fmt.Sprintf("%gdeg", degrees), func(t *testing.T) {
			transport := newFakeTransport()
			server := NewServer(transport, WithSleeper(func(time.Duration) {}))
			if err := server.Look(context.Background(), degrees, 0); err != nil {
				t.Fatal(err)
			}
			state := sentStates(t, transport)[0]
			got, _ := floatSlice(Resolve(state, []any{"hmd", "head", "orientation"}), 4)
			want := Slerp([]float64{0, 0, 0, 1}, QuatYaw(degrees*math.Pi/180), 1)
			if !closeVector(got, want, 1e-9) {
				t.Fatalf("orientation=%v want=%v", got, want)
			}
		})
	}
}

func TestEveryDeclaredInputPathPreservesItsPythonValueType(t *testing.T) {
	for hand, paths := range map[string][]string{"left": LeftInputPaths, "right": RightInputPaths} {
		for _, path := range paths {
			t.Run(hand+path, func(t *testing.T) {
				transport := newFakeTransport()
				server := NewServer(transport, WithSleeper(func(time.Duration) {}))
				var value any = true
				if hasNumericSuffix(path) {
					value = 0.625
				}
				if err := server.SetInput(context.Background(), hand, path, value, 0); err != nil {
					t.Fatal(err)
				}
				got := Resolve(sentStates(t, transport)[0], []any{hand, "inputs", path})
				if !valuesNear(got, value) {
					t.Fatalf("value=%v (%T), want=%v (%T)", got, got, value, value)
				}
			})
		}
	}
}

func TestSetInputDurationHoldsWithoutRelease(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithRate(4), WithSleeper(func(time.Duration) {}))
	if err := server.SetInput(context.Background(), "left", "/input/squeeze/value", 0.8, 500); err != nil {
		t.Fatal(err)
	}
	states := sentStates(t, transport)
	if len(states) != 2 {
		t.Fatalf("frames=%d", len(states))
	}
	for index, state := range states {
		if got := Resolve(state, []any{"left", "inputs", "/input/squeeze/value"}); got != 0.8 {
			t.Fatalf("frame %d value=%v", index, got)
		}
	}
}

func TestInvalidHandButtonPathAndValueAreRejectedPrecisely(t *testing.T) {
	server := NewServer(newFakeTransport(), WithSleeper(func(time.Duration) {}))
	for name, operation := range map[string]func() error{
		"hand":         func() error { return server.SetTrigger(context.Background(), "middle", 1, 0) },
		"button":       func() error { return server.Press(context.Background(), "right", "x", 0) },
		"input path":   func() error { return server.SetInput(context.Background(), "left", "/button/a/click", true, 0) },
		"numeric type": func() error { return server.SetInput(context.Background(), "left", "/input/trigger/value", true, 0) },
		"boolean type": func() error { return server.SetInput(context.Background(), "left", "/button/x/click", "yes", 0) },
	} {
		t.Run(name, func(t *testing.T) {
			if err := operation(); err == nil {
				t.Fatal("invalid operation was accepted")
			}
		})
	}
}

func TestPressEmitsTouchedClickThenReleaseAndWaitUsesMilliseconds(t *testing.T) {
	transport := newFakeTransport()
	var sleeps []time.Duration
	server := NewServer(transport, WithSleeper(func(duration time.Duration) { sleeps = append(sleeps, duration) }))
	if err := server.Press(context.Background(), "left", "x", 125); err != nil {
		t.Fatal(err)
	}
	states := sentStates(t, transport)
	for _, path := range []string{"/button/x/click", "/button/x/touch"} {
		if Resolve(states[0], []any{"left", "inputs", path}) != true || Resolve(states[1], []any{"left", "inputs", path}) != false {
			t.Fatalf("%s frames=%v", path, states)
		}
	}
	server.Wait(75)
	server.Wait(0)
	server.Wait(-75)
	if len(sleeps) != 2 || sleeps[0] != 125*time.Millisecond || sleeps[1] != 75*time.Millisecond {
		t.Fatalf("sleeps=%v", sleeps)
	}
}

func TestResolveAndCompareCoverPythonAssertionOperators(t *testing.T) {
	state := map[string]any{"items": []any{map[string]any{"slash/key": 2.0}}, "yes": true, "no": false}
	if got := Resolve(state, []any{"items", 0, "slash/key"}); got != 2.0 {
		t.Fatalf("resolved=%v", got)
	}
	if Resolve(state, []any{"items", -1}) != nil || Resolve(state, []any{"items", 9}) != nil || Resolve(state, []any{"missing"}) != nil {
		t.Fatal("invalid path did not resolve to nil")
	}
	for _, test := range []struct {
		actual, expected any
		op               string
		tol              float64
		want             bool
	}{
		{2.0, 2.01, "near", .02, true}, {2.0, 2.1, "near", .02, false},
		{2.0, 2, "eq", 0, true}, {2.0, 3, "ne", 0, true},
		{3.0, 2.0, "gt", 0, true}, {1.0, 2.0, "lt", 0, true},
		{true, nil, "true", 0, true}, {false, nil, "false", 0, true},
		{nil, nil, "eq", 0, true}, {"x", 0, "gt", 0, false}, {1, 1, "unknown", 0, false},
	} {
		if got := Compare(test.actual, test.op, test.expected, test.tol); got != test.want {
			t.Fatalf("Compare(%v,%s,%v)=%v want=%v", test.actual, test.op, test.expected, got, test.want)
		}
	}
}

func TestCompareCoercesBooleansLikePythonNumericOperators(t *testing.T) {
	// Python's _cmp ran every numeric operator through float(), and float(True)
	// is 1.0, so a bool input path answers near/eq/ne/gt/lt as 1/0. The whole
	// table is the Python result, not the Go implementation's own output: the
	// bool paths (/click, /touch) are exactly the ones scenarios assert with
	// "value 1"/"value 0", so a Go-only refusal to coerce turns a legitimate
	// press assertion into a timeout and an unpressed button into a passing
	// "ne 0".
	for _, test := range []struct {
		actual, expected any
		op               string
		want             bool
	}{
		{true, 1, "eq", true}, {true, 1.0, "eq", true}, {true, 0, "eq", false},
		{false, 0, "eq", true}, {false, 1, "eq", false}, {true, true, "eq", true},
		{true, "x", "eq", false}, {true, nil, "eq", false},
		{true, 0, "ne", true}, {false, 0, "ne", false},
		{true, 1, "ne", false}, {false, 1, "ne", true},
		{true, 0, "gt", true}, {false, 0, "gt", false},
		{true, 1, "lt", false}, {false, 1, "lt", true},
		{true, 1, "near", true}, {false, 0, "near", true}, {true, 0, "near", false},
		// true/false are identity checks in Python (`actual is True`), so they
		// stay type-strict and must not gain the numeric coercion above.
		{true, nil, "true", true}, {1.0, nil, "true", false},
		{false, nil, "false", true}, {0.0, nil, "false", false},
		// None is not numeric in Python either, and only "ne" is satisfied by it.
		{nil, 0, "ne", true}, {nil, 0, "eq", false}, {nil, 0, "near", false},
	} {
		if got := Compare(test.actual, test.op, test.expected, .01); got != test.want {
			t.Errorf("Compare(%#v,%s,%#v)=%v want=%v", test.actual, test.op, test.expected, got, test.want)
		}
	}
}

func valuesNear(got, want any) bool {
	gotNumber, gotIsNumber := asFloat(got)
	wantNumber, wantIsNumber := asFloat(want)
	if gotIsNumber && wantIsNumber {
		return math.Abs(gotNumber-wantNumber) <= 1e-9
	}
	return got == want
}

func TestScenarioSummary(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
	scenario := map[string]any{"steps": []any{
		map[string]any{"cmd": "hello", "role": "writer"},
		map[string]any{"cmd": "move_head", "to": map[string]any{"position": []any{0, 1.6, -2}}, "duration_ms": 0},
		map[string]any{"cmd": "assert", "get": []any{"hmd", "head", "position", 2}, "op": "near", "value": -2.0, "tol": .001},
	}}
	summary, err := server.RunScenario(context.Background(), scenario)
	if err != nil {
		t.Fatal(err)
	}
	if !boolValue(summary["ok"]) || summary["passed"] != 1 {
		t.Fatalf("summary = %v", summary)
	}
}

func TestScenarioFailureNamesAndInvalidInputs(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithSleeper(func(time.Duration) {}))
	summary, err := server.RunScenario(context.Background(), map[string]any{"steps": []any{
		map[string]any{"cmd": "assert", "name": "head should move", "get": []any{"hmd", "head", "position", 2}, "op": "eq", "value": -9.0},
		map[string]any{"cmd": "assert", "name": "head remains at zero", "get": []any{"hmd", "head", "position", 2}, "op": "eq", "value": 0.0},
	}})
	if err != nil || summary["asserts"] != 2 || summary["passed"] != 1 || summary["failed"] != 1 || summary["ok"] != false {
		t.Fatalf("summary=%v err=%v", summary, err)
	}
	failures, ok := summary["failures"].([]string)
	if !ok || len(failures) != 1 || failures[0] != "head should move" {
		t.Fatalf("failures=%#v", summary["failures"])
	}

	for name, scenario := range map[string]map[string]any{
		"steps not array": {"steps": "bad"},
		"step not object": {"steps": []any{"bad"}},
		"unknown command": {"steps": []any{map[string]any{"cmd": "unknown"}}},
	} {
		t.Run(name, func(t *testing.T) {
			server := NewServer(newFakeTransport(), WithSleeper(func(time.Duration) {}))
			if _, err := server.RunScenario(context.Background(), scenario); err == nil {
				t.Fatalf("scenario=%v was accepted", scenario)
			}
		})
	}
	if _, err := server.RunScenarioJSON(context.Background(), []byte("null")); err == nil {
		t.Fatal("null scenario was accepted")
	}
	if _, err := server.RunScenarioJSON(context.Background(), []byte("{")); err == nil {
		t.Fatal("malformed scenario was accepted")
	}
}

func TestScenarioWithoutStepsRunsHelloAndReturnsEmptySummary(t *testing.T) {
	for _, scenario := range []map[string]any{{}, {"steps": []any{}}} {
		transport := newFakeTransport()
		server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
		summary, err := server.RunScenario(context.Background(), scenario)
		if err != nil {
			t.Fatalf("scenario %v: %v", scenario, err)
		}
		if summary["ok"] != true || summary["asserts"] != 0 || summary["failures"] == nil {
			t.Fatalf("summary = %v", summary)
		}
		if len(transport.requests) < 2 || transport.requests[0]["cmd"] != "hello" || transport.requests[1]["cmd"] != "get_state" {
			t.Fatalf("requests = %v", transport.requests)
		}
	}
}

func TestRecordingRoundTripAndReplay(t *testing.T) {
	transport := newFakeTransport()
	recorder := NewRecorder(transport, 60)
	if err := recorder.Hello(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := recorder.Sample(context.Background(), time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(recorder.Frames) != 1 {
		t.Fatalf("frames = %d", len(recorder.Frames))
	}
	path := t.TempDir() + "/trace.json"
	if err := SaveRecording(path, recorder.Recording("test")); err != nil {
		t.Fatal(err)
	}
	recording, err := LoadRecording(path)
	if err != nil {
		t.Fatal(err)
	}
	replayer := NewReplayer(transport)
	if err := replayer.Hello(context.Background()); err != nil {
		t.Fatal(err)
	}
	count, err := replayer.Play(context.Background(), recording, nil)
	if err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("replayed frames = %d", count)
	}
}

func TestDirectSetStateUsesCompleteSnapshot(t *testing.T) {
	transport := newFakeTransport()
	server := NewServer(transport, WithSleeper(func(_ time.Duration) {}))
	if err := server.Hello(context.Background(), "writer"); err != nil {
		t.Fatal(err)
	}
	state := DefaultModel().Snapshot(42).Raw()
	state["hmd"].(map[string]any)["head"].(map[string]any)["position"].([]any)[2] = -4.0
	if err := server.SetState(context.Background(), state); err != nil {
		t.Fatal(err)
	}
	got, err := server.GetState(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if Resolve(got, []any{"hmd", "head", "position", 2}) != -4.0 {
		t.Fatalf("state = %v", got)
	}
}

func TestSessionRecordStatus(t *testing.T) {
	path := t.TempDir() + "/session.json"
	record := SessionRecord{PID: os.Getpid(), Command: "test", StartedAt: time.Now().UTC()}
	if err := SaveSession(path, record); err != nil {
		t.Fatal(err)
	}
	loaded, err := LoadSession(path)
	if err != nil || loaded.PID != record.PID {
		t.Fatalf("loaded=%v err=%v", loaded, err)
	}
	status, err := SessionStatus(path)
	if err != nil || status["running"] != true {
		t.Fatalf("status=%v err=%v", status, err)
	}
}

func TestSessionStartStatusStopLifecycle(t *testing.T) {
	if os.Getenv("PLAYSPECTRA_SESSION_HELPER") == "1" {
		time.Sleep(30 * time.Second)
		return
	}
	pidFile := t.TempDir() + "/session.json"
	t.Setenv("PLAYSPECTRA_SESSION_HELPER", "1")
	record, err := StartSession(pidFile, os.Args[0], "-test.run=TestSessionStartStatusStopLifecycle")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = StopSession(pidFile) }()
	if record.PID <= 0 || record.Command != os.Args[0] || len(record.Arguments) != 1 {
		t.Fatalf("record=%+v", record)
	}
	deadline := time.Now().Add(5 * time.Second)
	for !ProcessRunning(record.PID) && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	status, err := SessionStatus(pidFile)
	if err != nil || status["running"] != true {
		t.Fatalf("status=%v err=%v", status, err)
	}
	if _, err := StartSession(pidFile, os.Args[0]); err == nil {
		t.Fatal("second session start was accepted")
	}
	if err := StopSession(pidFile); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(pidFile); !os.IsNotExist(err) {
		t.Fatalf("pid file remains: %v", err)
	}
}
