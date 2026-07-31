package playspectra

import (
	"context"
	"errors"
	"os"
	"testing"
	"time"
)

var headZPath = []any{"hmd", "head", "position", 2}

type changingStateTransport struct {
	*fakeTransport
	getCount int
	changeAt int
	changeTo float64
}

func newChangingStateTransport() *changingStateTransport {
	return &changingStateTransport{fakeTransport: newFakeTransport()}
}

func (f *changingStateTransport) Request(ctx context.Context, request map[string]any) (map[string]any, error) {
	if request["cmd"] == "get_state" {
		f.getCount++
		if f.changeAt > 0 && f.getCount == f.changeAt {
			f.state["hmd"].(map[string]any)["head"].(map[string]any)["position"].([]any)[2] = f.changeTo
		}
	}
	return f.fakeTransport.Request(ctx, request)
}

// This test maps to all checks in tools/playspectra_waitfor_test.py while
// replacing wall-clock thread scheduling with a deterministic state change
// after a known poll count.
func TestPythonWaitForAndRetryingAssertCharacterization(t *testing.T) {
	ctx := context.Background()
	transport := newChangingStateTransport()
	server := NewServer(transport)

	if ok, err := server.AssertState(ctx, headZPath, "near", 0.0, 1e-6, "", 0, 25); err != nil || !ok {
		t.Fatalf("single-shot true condition: ok=%v err=%v", ok, err)
	}
	started := time.Now()
	if ok, err := server.AssertState(ctx, headZPath, "near", -2.0, 1e-6, "", 0, 25); err != nil || ok {
		t.Fatalf("single-shot false condition: ok=%v err=%v", ok, err)
	}
	if elapsed := time.Since(started); elapsed >= 100*time.Millisecond {
		t.Fatalf("single-shot assertion took %v", elapsed)
	}

	transport.changeAt = transport.getCount + 4
	transport.changeTo = -2
	started = time.Now()
	if ok, err := server.WaitFor(ctx, headZPath, "near", -2.0, 1e-6, 1000, 5, ""); err != nil || !ok {
		t.Fatalf("wait_for delayed change: ok=%v err=%v", ok, err)
	}
	if elapsed := time.Since(started); elapsed < 10*time.Millisecond {
		t.Fatalf("wait_for did not poll/wait: %v", elapsed)
	}

	transport.changeAt = 0
	started = time.Now()
	if ok, err := server.WaitFor(ctx, headZPath, "near", 999.0, 1e-6, 35, 5, ""); err != nil || ok {
		t.Fatalf("wait_for impossible condition: ok=%v err=%v", ok, err)
	}
	if elapsed := time.Since(started); elapsed < 25*time.Millisecond || elapsed > 500*time.Millisecond {
		t.Fatalf("timeout deadline not honoured: %v", elapsed)
	}

	transport.state["hmd"].(map[string]any)["head"].(map[string]any)["position"].([]any)[2] = 0.0
	if ok, _ := server.AssertState(ctx, headZPath, "near", -3.0, 1e-6, "", 0, 25); ok {
		t.Fatal("single-shot assert passed before delayed change")
	}
	transport.changeAt = transport.getCount + 3
	transport.changeTo = -3
	if ok, err := server.AssertState(ctx, headZPath, "near", -3.0, 1e-6, "", 1000, 5); err != nil || !ok {
		t.Fatalf("retrying assert: ok=%v err=%v", ok, err)
	}

	transport.state["hmd"].(map[string]any)["head"].(map[string]any)["position"].([]any)[2] = 0.0
	transport.changeAt = transport.getCount + 4 // hello seed + three wait polls
	transport.changeTo = -1.5
	server.ResetAssertions()
	summary, err := server.RunScenario(ctx, map[string]any{"steps": []any{
		map[string]any{"cmd": "wait_for", "get": headZPath, "op": "near", "value": -1.5, "tol": 1e-6, "timeout_ms": 1000, "poll_ms": 5},
		map[string]any{"cmd": "assert", "get": headZPath, "op": "near", "value": -1.5, "tol": 1e-6},
	}})
	if err != nil || summary["ok"] != true || summary["asserts"] != 2 || summary["passed"] != 2 {
		t.Fatalf("scenario summary=%v err=%v", summary, err)
	}
}

type changingCapture struct {
	path       string
	requests   int
	changeAt   int
	changeData []byte
	response   map[string]any
	err        error
}

func (c *changingCapture) RequestLine(_ context.Context, request map[string]any) (map[string]any, error) {
	c.requests++
	if c.changeAt > 0 && c.requests == c.changeAt {
		if err := os.WriteFile(c.path, c.changeData, 0o600); err != nil {
			return nil, err
		}
	}
	if c.err != nil {
		return nil, c.err
	}
	if c.response != nil {
		return c.response, nil
	}
	return map[string]any{"ok": true, "path": c.path, "eye": request["eye"]}, nil
}

// This test maps to all checks in tools/playspectra_capture_assert_test.py.
func TestPythonCaptureAssertionCharacterization(t *testing.T) {
	path := t.TempDir() + "/shot.png"
	baseline := []byte("\x89PNG\r\n\x1a\nAAAA")
	if err := os.WriteFile(path, baseline, 0o600); err != nil {
		t.Fatal(err)
	}
	capture := &changingCapture{path: path}
	server := NewServer(newFakeTransport(), WithCapture(capture))
	ctx := context.Background()

	if !server.CaptureRef(ctx, "base", "left") {
		t.Fatal("capture_ref did not store reference")
	}
	if !server.AssertCapture(ctx, "base", "stable", "left", "", 0, 50) {
		t.Fatal("stable assertion failed for identical frame")
	}
	if err := os.WriteFile(path, []byte("\x89PNG\r\n\x1a\nBBBB"), 0o600); err != nil {
		t.Fatal(err)
	}
	if !server.AssertCapture(ctx, "base", "changed", "left", "", 0, 50) {
		t.Fatal("changed assertion failed after byte swap")
	}
	if !server.CaptureRef(ctx, "base2", "left") {
		t.Fatal("second capture_ref failed")
	}
	started := time.Now()
	if server.AssertCapture(ctx, "base2", "changed", "left", "", 0, 50) {
		t.Fatal("single-shot changed assertion passed without a change")
	}
	if elapsed := time.Since(started); elapsed >= 200*time.Millisecond {
		t.Fatalf("single-shot capture assertion took %v", elapsed)
	}

	capture.changeAt = capture.requests + 3
	capture.changeData = []byte("\x89PNG\r\n\x1a\nCCCC")
	started = time.Now()
	if !server.AssertCapture(ctx, "base2", "changed", "left", "", 1000, 10) {
		t.Fatal("retrying capture assertion missed delayed change")
	}
	if elapsed := time.Since(started); elapsed < 15*time.Millisecond {
		t.Fatalf("retrying capture assertion did not wait: %v", elapsed)
	}

	if !server.CaptureRef(ctx, "base3", "left") {
		t.Fatal("third capture_ref failed")
	}
	capture.changeAt = 0
	started = time.Now()
	if server.AssertCapture(ctx, "base3", "changed", "left", "", 35, 5) {
		t.Fatal("capture assertion passed when frame never changed")
	}
	if elapsed := time.Since(started); elapsed < 25*time.Millisecond || elapsed > 500*time.Millisecond {
		t.Fatalf("capture timeout deadline not honoured: %v", elapsed)
	}
}

func TestScreenshotFailureModesAreStructured(t *testing.T) {
	ctx := context.Background()
	server := NewServer(newFakeTransport())
	if got, err := server.Screenshot(ctx, "left", 10); err != nil || got["ok"] != false || got["error"] != "no capture channel (need --capture-port + a layer-loaded app)" {
		t.Fatalf("no capture result=%v err=%v", got, err)
	}

	capture := &changingCapture{err: errors.New("capture unavailable")}
	server = NewServer(newFakeTransport(), WithCapture(capture))
	if got, err := server.Screenshot(ctx, "left", 10); err != nil || got["ok"] != false {
		t.Fatalf("request error result=%v err=%v", got, err)
	}

	capture.err = nil
	capture.response = map[string]any{"ok": false, "error": "not ready"}
	if got, err := server.Screenshot(ctx, "left", 10); err != nil || got["ok"] != false {
		t.Fatalf("not-ok result=%v err=%v", got, err)
	}

	capture.response = map[string]any{"ok": true, "path": t.TempDir() + "/missing.png"}
	if got, err := server.Screenshot(ctx, "left", 10); err != nil || got["error"] != "screenshot path missing: "+capture.response["path"].(string) {
		t.Fatalf("missing path result=%v err=%v", got, err)
	}

	capture.response = map[string]any{"ok": true, "path": t.TempDir()}
	if got, err := server.Screenshot(ctx, "left", 10); err != nil || got["ok"] != false || got["error"] == nil {
		t.Fatalf("read error result=%v err=%v", got, err)
	}

	server = NewServer(newFakeTransport(), WithCapture(&changingCapture{path: t.TempDir() + "/unused.png"}))
	if server.AssertCapture(ctx, "missing", "changed", "left", "named missing ref", 0, 10) {
		t.Fatal("missing capture reference passed")
	}
	assertions := server.Assertions()
	if len(assertions) != 1 || assertions[0].Name != "named missing ref" || assertions[0].OK {
		t.Fatalf("assertions=%v", assertions)
	}
}
