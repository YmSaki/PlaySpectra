package verify

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"testing"
)

func TestReportTextAndExitCode(t *testing.T) {
	report := NewReport("fixture")
	report.Add("one", true, "ok")
	report.Add("two", false, "bad")
	if report.Passed() != 1 || report.OK() || report.ExitCode() != 1 {
		t.Fatalf("report = %+v", report)
	}
	var output bytes.Buffer
	report.WriteText(&output)
	if !strings.Contains(output.String(), "PASS one - ok") || !strings.Contains(output.String(), "=== 1/2 PASS ===") {
		t.Fatalf("output = %q", output.String())
	}
}

func TestFrameSynchronizedProbeRetainsTenPythonChecks(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	done := make(chan error, 1)
	go serveFrameFixture(listener, done)
	report, err := FrameSynchronized(context.Background(), listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Checks) != 10 || !report.OK() {
		t.Fatalf("report = %+v", report)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func serveFrameFixture(listener net.Listener, done chan<- error) {
	connection, err := listener.Accept()
	if err != nil {
		done <- err
		return
	}
	defer connection.Close()
	stateZ := 0.0
	lastFrame := -1
	lastZ := 0.0
	scanner := bufio.NewScanner(connection)
	for scanner.Scan() {
		var request map[string]any
		if err := json.Unmarshal(scanner.Bytes(), &request); err != nil {
			done <- err
			return
		}
		response := map[string]any{"request_id": request["request_id"], "ok": true}
		switch request["cmd"] {
		case "hello":
			response["role_granted"] = request["role"]
		case "get_state":
			response["state"] = map[string]any{"hmd": map[string]any{"head": map[string]any{"position": []float64{0, 1.6, stateZ}}}}
		case "set_state":
			state := request["state"].(map[string]any)
			logical, _ := number(resolve(state, "clock", "logical_frame"))
			z, _ := number(resolve(state, "hmd", "head", "position", 2))
			frame := int(logical)
			switch {
			case frame > lastFrame:
				lastFrame, lastZ, stateZ = frame, z, z
				response["applied"], response["logical_frame"] = true, frame
			case frame == lastFrame && z == lastZ:
				response["applied"], response["idempotent"], response["logical_frame"] = true, true, frame
			case frame == lastFrame:
				response["ok"], response["error_type"], response["error"], response["logical_frame"] = false, "conflict_error", "frame_content_mismatch", frame
			default:
				response["applied"], response["reason"] = false, "stale_frame"
			}
		}
		data, _ := json.Marshal(response)
		if _, err := fmt.Fprintf(connection, "%s\n", data); err != nil {
			done <- err
			return
		}
	}
	done <- scanner.Err()
}
