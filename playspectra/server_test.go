package playspectra

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"
)

type fakeTransport struct {
	state    map[string]any
	requests []map[string]any
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
	transport := newFakeTransport()
	server := NewServer(transport, WithRate(10), WithSleeper(func(_ time.Duration) {}))
	if err := server.MoveHead(context.Background(), map[string]any{}, 250); err != nil {
		t.Fatal(err)
	}
	frames := 0
	for _, request := range transport.requests {
		if request["cmd"] == "set_state" {
			frames++
		}
	}
	if frames != 2 {
		t.Fatalf("set_state frames = %d, want 2 for round(2.5)", frames)
	}
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
