package playspectra

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"
)

type fixedReplyTransport struct{ response map[string]any }

func (f fixedReplyTransport) Request(context.Context, map[string]any) (map[string]any, error) {
	return f.response, nil
}
func (fixedReplyTransport) SendOnly(context.Context, map[string]any) error { return nil }

func TestEmptyRecordingSerializesFramesAsArray(t *testing.T) {
	recording := NewRecorder(newFakeTransport(), 60).Recording("empty")
	data, err := json.Marshal(recording)
	if err != nil {
		t.Fatal(err)
	}
	var raw map[string]any
	if err := json.Unmarshal(data, &raw); err != nil {
		t.Fatal(err)
	}
	frames, ok := raw["frames"].([]any)
	if !ok || len(frames) != 0 {
		t.Fatalf("frames = %#v, want []", raw["frames"])
	}
}

func TestRecorderUsesObserverRoleAndMonotonicSampleTimes(t *testing.T) {
	transport := newFakeTransport()
	recorder := NewRecorder(transport, 120)
	if err := recorder.Hello(context.Background()); err != nil {
		t.Fatal(err)
	}
	started := time.Now().Add(-time.Second)
	if err := recorder.Sample(context.Background(), started); err != nil {
		t.Fatal(err)
	}
	if err := recorder.Sample(context.Background(), started); err != nil {
		t.Fatal(err)
	}
	if transport.requests[0]["cmd"] != "hello" || transport.requests[0]["role"] != "observer" || transport.requests[0]["protocol_version"] != 1 {
		t.Fatalf("hello request = %v", transport.requests[0])
	}
	if len(recorder.Frames) != 2 || recorder.Frames[0].TMS < 900 || recorder.Frames[1].TMS < recorder.Frames[0].TMS {
		t.Fatalf("frames = %v", recorder.Frames)
	}
}

func TestRecorderAndReplayerRejectWrongGrantedRole(t *testing.T) {
	if err := NewRecorder(fixedReplyTransport{response: map[string]any{"ok": true, "role_granted": "writer"}}, 60).Hello(context.Background()); err == nil {
		t.Fatal("recorder accepted writer role")
	}
	if err := NewReplayer(fixedReplyTransport{response: map[string]any{"ok": true, "role_granted": "observer"}}).Hello(context.Background()); err == nil {
		t.Fatal("replayer accepted observer role")
	}
}

func TestRecorderRejectsStateLessSample(t *testing.T) {
	recorder := NewRecorder(fixedReplyTransport{response: map[string]any{"ok": true}}, 60)
	if err := recorder.Sample(context.Background(), time.Now()); err == nil {
		t.Fatal("state-less sample was silently recorded")
	}
	if len(recorder.Frames) != 0 {
		t.Fatalf("frames = %v", recorder.Frames)
	}
}

func TestRecordForZeroDurationDoesNotSample(t *testing.T) {
	transport := newFakeTransport()
	recorder := NewRecorder(transport, 60)
	if err := recorder.RecordFor(context.Background(), 0); err != nil {
		t.Fatal(err)
	}
	if len(recorder.Frames) != 0 || len(transport.requests) != 0 {
		t.Fatalf("frames=%v requests=%v", recorder.Frames, transport.requests)
	}
}

func TestRecorderRunStopsLikePythonBackgroundRecorder(t *testing.T) {
	recorder := NewRecorder(newFakeTransport(), 1000)
	done := make(chan error, 1)
	go func() { done <- recorder.Run(context.Background(), time.Now()) }()
	time.Sleep(8 * time.Millisecond)
	recorder.Stop()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("background recorder did not stop")
	}
	if len(recorder.Frames) == 0 {
		t.Fatal("background recorder captured no frames")
	}
	count := len(recorder.Frames)
	time.Sleep(5 * time.Millisecond)
	if len(recorder.Frames) != count {
		t.Fatalf("frames continued after Stop: %d -> %d", count, len(recorder.Frames))
	}
}

func TestRecordForHonorsPriorStop(t *testing.T) {
	recorder := NewRecorder(newFakeTransport(), 1000)
	recorder.Stop()
	if err := recorder.RecordFor(context.Background(), time.Second); err != nil {
		t.Fatal(err)
	}
	if len(recorder.Frames) != 0 {
		t.Fatalf("frames=%v", recorder.Frames)
	}
}

func TestReplayerUsesFreshMonotonicSequenceAndDoesNotMutateRecording(t *testing.T) {
	transport := newFakeTransport()
	transport.state["sequence"] = float64(50)
	original := DefaultModel().Snapshot(7).Raw()
	original["clock"] = map[string]any{"mode": "frame_synchronized", "logical_frame": float64(9)}
	recording := Recording{Name: "fixture", RateHz: 60, Frames: []RecordingFrame{
		{TMS: 10, State: original},
		{TMS: 10, State: original},
	}}
	replayer := NewReplayer(transport)
	if err := replayer.Hello(context.Background()); err != nil {
		t.Fatal(err)
	}
	count, err := replayer.Play(context.Background(), recording, nil)
	if err != nil || count != 2 {
		t.Fatalf("count=%d err=%v", count, err)
	}
	states := sentStates(t, transport)
	if len(states) != 2 {
		t.Fatalf("states = %v", states)
	}
	for index, state := range states {
		if state["sequence"] != float64(51+index) {
			t.Fatalf("frame %d sequence = %v", index, state["sequence"])
		}
		if state["protocol_version"] != float64(1) {
			t.Fatalf("frame %d protocol_version = %v", index, state["protocol_version"])
		}
		clock := state["clock"].(map[string]any)
		if clock["mode"] != "realtime" || len(clock) != 1 {
			t.Fatalf("frame %d clock = %v", index, clock)
		}
	}
	if original["sequence"] != float64(7) || original["clock"].(map[string]any)["mode"] != "frame_synchronized" {
		t.Fatalf("recording frame was mutated: %v", original)
	}
}

func TestReplayerPreservesRelativeFrameTiming(t *testing.T) {
	transport := newFakeTransport()
	state := DefaultModel().Snapshot(1).Raw()
	recording := Recording{Frames: []RecordingFrame{{TMS: 100, State: state}, {TMS: 130, State: state}}}
	started := time.Now()
	count, err := NewReplayer(transport).Play(context.Background(), recording, nil)
	elapsed := time.Since(started)
	if err != nil || count != 2 {
		t.Fatalf("count=%d err=%v", count, err)
	}
	if elapsed < 20*time.Millisecond || elapsed > 500*time.Millisecond {
		t.Fatalf("relative 30ms replay took %v", elapsed)
	}
}

func TestReplayerEmptyRecordingDoesNotTouchTransport(t *testing.T) {
	transport := newFakeTransport()
	count, err := NewReplayer(transport).Play(context.Background(), Recording{Frames: []RecordingFrame{}}, nil)
	if err != nil || count != 0 || len(transport.requests) != 0 {
		t.Fatalf("count=%d err=%v requests=%v", count, err, transport.requests)
	}
}

func TestRecordingFileRoundTripIsSemanticJSON(t *testing.T) {
	path := t.TempDir() + "/trace.json"
	recording := Recording{Name: "trace", RateHz: 90, Frames: []RecordingFrame{{TMS: 1.25, State: DefaultModel().Snapshot(3).Raw()}}}
	if err := SaveRecording(path, recording); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) == 0 || data[len(data)-1] != '\n' {
		t.Fatalf("recording has no trailing newline: %q", data)
	}
	loaded, err := LoadRecording(path)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Name != recording.Name || loaded.RateHz != recording.RateHz || len(loaded.Frames) != 1 || loaded.Frames[0].TMS != 1.25 {
		t.Fatalf("loaded = %+v", loaded)
	}
}
