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
