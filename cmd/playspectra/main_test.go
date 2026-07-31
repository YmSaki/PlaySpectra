package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"testing"

	core "github.com/YmSaki/PlaySpectra/playspectra"
)

func TestPythonCLICompatibleOperationDefaults(t *testing.T) {
	tests := []struct {
		operation string
		duration  int
		hand      string
	}{
		{"move_head", 500, "left"},
		{"look", 500, "left"},
		{"walk_forward", 1000, "left"},
		{"strafe", 1000, "left"},
		{"trigger", 200, "right"},
		{"set_trigger", 200, "right"},
		{"move_controller", 400, "right"},
		{"set_input", 0, "right"},
		{"press", 0, "right"},
	}
	for _, test := range tests {
		t.Run(test.operation, func(t *testing.T) {
			options := defaultCommandOptions(test.operation)
			if options.DurationMS != test.duration || options.Hand != test.hand {
				t.Fatalf("options = %+v, want duration=%d hand=%s", options, test.duration, test.hand)
			}
		})
	}
}

func TestOperationArgumentsCoverFlagDrivenCommands(t *testing.T) {
	tests := []struct {
		operation string
		options   commandOptions
		want      map[string]any
	}{
		{"move_head", commandOptions{X: 1, Y: 2, Z: 3, DurationMS: 4}, map[string]any{"to": map[string]any{"position": []any{1.0, 2.0, 3.0}}, "duration_ms": 4}},
		{"look", commandOptions{YawDeg: 90, DurationMS: 5}, map[string]any{"yaw_deg": 90.0, "duration_ms": 5}},
		{"walk_forward", commandOptions{Speed: .5, DurationMS: 6, Hand: "left"}, map[string]any{"speed": .5, "duration_ms": 6, "hand": "left"}},
		{"strafe", commandOptions{Speed: -.5, DurationMS: 7, Hand: "left"}, map[string]any{"speed": -.5, "duration_ms": 7, "hand": "left"}},
		{"trigger", commandOptions{Value: .8, DurationMS: 8, Hand: "right"}, map[string]any{"value": .8, "duration_ms": 8, "hand": "right"}},
		{"move_controller", commandOptions{X: 1, Y: 2, Z: 3, DurationMS: 9, Hand: "right"}, map[string]any{"hand": "right", "to": map[string]any{"position": []any{1.0, 2.0, 3.0}}, "duration_ms": 9}},
		{"set_input", commandOptions{Hand: "right", Path: "/input/squeeze/value", Value: .6, DurationMS: 10}, map[string]any{"hand": "right", "path": "/input/squeeze/value", "value": .6, "duration_ms": 10}},
		{"press", commandOptions{Hand: "right", Button: "a", MS: 11}, map[string]any{"hand": "right", "button": "a", "ms": 11}},
		{"reset", commandOptions{}, map[string]any{}},
	}
	for _, test := range tests {
		t.Run(test.operation, func(t *testing.T) {
			if got := operationArguments(test.operation, test.options); !reflect.DeepEqual(got, test.want) {
				t.Fatalf("arguments = %#v, want %#v", got, test.want)
			}
		})
	}
}

func TestCommandAliasesNormalizeToScenarioNames(t *testing.T) {
	for input, want := range map[string]string{
		"move-head": "move_head", "move_head": "move_head", "get-state": "get_state", "wait-for": "wait_for",
	} {
		if got := normalizeCommand(input); got != want {
			t.Fatalf("normalizeCommand(%q) = %q, want %q", input, got, want)
		}
	}
}

type cliAdapter struct {
	listener net.Listener
	state    map[string]any
	mu       sync.Mutex
	requests []map[string]any
	done     chan error
}

func startCLIAdapter(t *testing.T) *cliAdapter {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	adapter := &cliAdapter{listener: listener, state: core.DefaultModel().Snapshot(0).Raw(), done: make(chan error, 1)}
	go adapter.serve()
	return adapter
}

func (a *cliAdapter) port() int { return a.listener.Addr().(*net.TCPAddr).Port }

func (a *cliAdapter) serve() {
	conn, err := a.listener.Accept()
	if err != nil {
		a.done <- err
		return
	}
	defer conn.Close()
	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		var request map[string]any
		if err := json.Unmarshal(scanner.Bytes(), &request); err != nil {
			a.done <- err
			return
		}
		a.mu.Lock()
		a.requests = append(a.requests, request)
		a.mu.Unlock()
		response := map[string]any{"ok": true, "request_id": request["request_id"]}
		switch request["cmd"] {
		case "hello":
			response["role_granted"] = request["role"]
		case "get_state":
			response["state"] = a.state
		case "set_state":
			if state, ok := request["state"].(map[string]any); ok {
				a.state = state
			}
		}
		data, _ := json.Marshal(response)
		if _, err := fmt.Fprintf(conn, "%s\n", data); err != nil {
			a.done <- err
			return
		}
	}
	a.done <- scanner.Err()
}

func (a *cliAdapter) close(t *testing.T) {
	t.Helper()
	_ = a.listener.Close()
	if err := <-a.done; err != nil {
		t.Fatal(err)
	}
}

func (a *cliAdapter) count(command string) int {
	a.mu.Lock()
	defer a.mu.Unlock()
	count := 0
	for _, request := range a.requests {
		if request["cmd"] == command {
			count++
		}
	}
	return count
}

func captureRun(t *testing.T, args ...string) (int, string, string) {
	t.Helper()
	stdoutReader, stdoutWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	stderrReader, stderrWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	originalStdout, originalStderr := os.Stdout, os.Stderr
	os.Stdout, os.Stderr = stdoutWriter, stderrWriter
	code := run(args)
	_ = stdoutWriter.Close()
	_ = stderrWriter.Close()
	os.Stdout, os.Stderr = originalStdout, originalStderr
	stdout, _ := io.ReadAll(stdoutReader)
	stderr, _ := io.ReadAll(stderrReader)
	_ = stdoutReader.Close()
	_ = stderrReader.Close()
	return code, string(stdout), string(stderr)
}

func TestCLIGetStateJSONContract(t *testing.T) {
	adapter := startCLIAdapter(t)
	code, stdout, stderr := captureRun(t, "cmd", "get-state", "--port", strconv.Itoa(adapter.port()))
	adapter.close(t)
	if code != 0 || stderr != "" {
		t.Fatalf("code=%d stderr=%q", code, stderr)
	}
	var output map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(stdout)), &output); err != nil {
		t.Fatalf("stdout is not one JSON object: %q: %v", stdout, err)
	}
	if output["cmd"] != "get_state" || output["result"] != nil || output["state"] == nil {
		t.Fatalf("output = %v", output)
	}
	if adapter.count("hello") != 1 || adapter.count("get_state") != 2 {
		t.Fatalf("requests = %v", adapter.requests)
	}
}

func TestCLIMoveHeadUsesPythonDefaultDuration(t *testing.T) {
	adapter := startCLIAdapter(t)
	code, stdout, stderr := captureRun(t, "cmd", "move-head", "--z", "-2", "--port", strconv.Itoa(adapter.port()))
	adapter.close(t)
	if code != 0 || !strings.Contains(stderr, "step: move_head") {
		t.Fatalf("code=%d stderr=%q", code, stderr)
	}
	var output map[string]any
	if err := json.Unmarshal([]byte(stdout), &output); err != nil {
		t.Fatal(err)
	}
	if got := core.Resolve(output["state"], []any{"hmd", "head", "position", 2}); got != -2.0 {
		t.Fatalf("head z = %v", got)
	}
	if got := adapter.count("set_state"); got != 30 {
		t.Fatalf("set_state frames = %d, want 30 for 500ms at 60Hz", got)
	}
}

func TestCLIFailedAssertionUsesExitOneWithJSONOutput(t *testing.T) {
	adapter := startCLIAdapter(t)
	args := `{"get":["hmd","head","position",2],"op":"near","value":-99,"timeout_ms":0}`
	code, stdout, stderr := captureRun(t, "cmd", "assert", "--args", args, "--port", strconv.Itoa(adapter.port()))
	adapter.close(t)
	if code != 1 || !strings.Contains(stderr, "step: assert") {
		t.Fatalf("code=%d stderr=%q", code, stderr)
	}
	var output map[string]any
	if err := json.Unmarshal([]byte(stdout), &output); err != nil {
		t.Fatal(err)
	}
	if output["result"] != false || output["state"] == nil {
		t.Fatalf("output = %v", output)
	}
}

func TestCLIBadArgsJSONUsesExitTwoWithoutConnecting(t *testing.T) {
	code, stdout, stderr := captureRun(t, "cmd", "look", "--args", "{")
	if code != 2 || stdout != "" || !strings.Contains(stderr, "bad --args JSON") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestImageStatsCLICompatibilityOutput(t *testing.T) {
	path := t.TempDir() + "/not-image.png"
	if err := os.WriteFile(path, []byte("not png"), 0o600); err != nil {
		t.Fatal(err)
	}
	code, stdout, stderr := captureRun(t, "image-stats", path)
	if code != 0 || stderr != "" || !strings.Contains(stdout, `"error":"not a PNG"`) || !strings.Contains(stdout, "non-degenerate: False") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
	missing := t.TempDir() + "/missing.png"
	code, stdout, stderr = captureRun(t, "image-stats", missing)
	if code != 0 || stderr != "" || stdout != missing+": missing\n" {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
	code, stdout, stderr = captureRun(t, "image-stats")
	if code != 2 || stdout != "" || !strings.Contains(stderr, "usage: playspectra image-stats") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestVerifyVRAppMissingExecutableIsExplicitSkip(t *testing.T) {
	missing := t.TempDir() + "/missing.exe"
	code, stdout, stderr := captureRun(t, "verify", "vrapp", "--exe", missing)
	if code != 0 || stderr != "" || !strings.Contains(stdout, "SKIP: VRAppDummyGame not built") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestVerifyCommandRejectsMissingAndUnknownSuite(t *testing.T) {
	for _, args := range [][]string{{"verify"}, {"verify", "unknown"}} {
		code, stdout, stderr := captureRun(t, args...)
		if code != 2 || stdout != "" || stderr == "" {
			t.Fatalf("args=%v code=%d stdout=%q stderr=%q", args, code, stdout, stderr)
		}
	}
}

func TestVerifyMCPReportsProcessStartFailure(t *testing.T) {
	missing := t.TempDir() + "/missing-playspectra"
	code, stdout, stderr := captureRun(t, "verify", "mcp", "--executable", missing)
	if code != 2 || stdout != "" || stderr == "" {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}
