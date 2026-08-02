package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
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

func TestMCPPortEnvironmentDefaultsMatchPython(t *testing.T) {
	t.Setenv("PLAYSPECTRA_MONADO_PORT", "60002")
	t.Setenv("PLAYSPECTRA_PORT", "60000")
	if got := envInt("PLAYSPECTRA_MONADO_PORT", 52702); got != 60002 {
		t.Fatalf("operate port=%d", got)
	}
	if got := envInt("PLAYSPECTRA_PORT", 52700); got != 60000 {
		t.Fatalf("capture port=%d", got)
	}
	t.Setenv("PLAYSPECTRA_PORT", "bad")
	if got := envInt("PLAYSPECTRA_PORT", 52700); got != 52700 {
		t.Fatalf("invalid env fallback=%d", got)
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

func TestEveryCompatibleCLICommandKeepsJSONStdoutContract(t *testing.T) {
	stateJSON, err := json.Marshal(core.DefaultModel().Snapshot(0).Raw())
	if err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name     string
		args     []string
		wantCode int
	}{
		{"hello", []string{"hello"}, 0},
		{"move_head", []string{"move-head", "--duration-ms", "0"}, 0},
		{"look", []string{"look", "--duration-ms", "0"}, 0},
		{"walk_forward", []string{"walk-forward", "--duration-ms", "0"}, 0},
		{"strafe", []string{"strafe", "--duration-ms", "0"}, 0},
		{"trigger", []string{"trigger", "--duration-ms", "0"}, 0},
		{"set_trigger", []string{"set-trigger", "--duration-ms", "0"}, 0},
		{"move_controller", []string{"move-controller", "--duration-ms", "0"}, 0},
		{"set_input", []string{"set-input", "--path", "/input/squeeze/value", "--duration-ms", "0"}, 0},
		{"press", []string{"press", "--ms", "0"}, 0},
		{"wait", []string{"wait", "--args", `{"ms":0}`}, 0},
		{"reset", []string{"reset"}, 0},
		{"set_state", []string{"set-state", "--args", `{"state":` + string(stateJSON) + `}`}, 0},
		{"get_state", []string{"get-state"}, 0},
		{"status", []string{"status"}, 0},
		{"assert", []string{"assert", "--args", `{"get":["hmd","head","position",2],"op":"eq","value":0}`}, 0},
		{"wait_for", []string{"wait-for", "--args", `{"get":["hmd","head","position",2],"op":"eq","value":0,"timeout_ms":0}`}, 0},
		{"capture", []string{"capture"}, 0},
		{"assert_capture", []string{"assert-capture", "--args", `{"ref":"missing","timeout_ms":0}`}, 1},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			adapter := startCLIAdapter(t)
			args := append([]string{"cmd"}, test.args...)
			args = append(args, "--port", strconv.Itoa(adapter.port()))
			code, stdout, _ := captureRun(t, args...)
			adapter.close(t)
			if code != test.wantCode {
				t.Fatalf("code=%d, want %d; stdout=%q", code, test.wantCode, stdout)
			}
			trimmed := strings.TrimSpace(stdout)
			if strings.Count(trimmed, "\n") != 0 {
				t.Fatalf("stdout contains more than one line: %q", stdout)
			}
			var output map[string]any
			if err := json.Unmarshal([]byte(trimmed), &output); err != nil {
				t.Fatalf("stdout is not one JSON object: %q: %v", stdout, err)
			}
			if output["cmd"] != normalizeCommand(test.args[0]) || output["state"] == nil {
				t.Fatalf("output=%v", output)
			}
		})
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

func TestCLIAssertionCommandsUseZeroOneTwoExitContract(t *testing.T) {
	for _, test := range []struct {
		name      string
		operation string
		args      string
		wantCode  int
	}{
		{"assert pass", "assert", `{"get":["hmd","head","position",2],"op":"near","value":0,"timeout_ms":0}`, 0},
		{"wait_for fail", "wait_for", `{"get":["hmd","head","position",2],"op":"eq","value":-1,"timeout_ms":0}`, 1},
		{"capture fail", "assert_capture", `{"ref":"missing","timeout_ms":0}`, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			adapter := startCLIAdapter(t)
			code, stdout, stderr := captureRun(t, "cmd", test.operation, "--args", test.args, "--port", strconv.Itoa(adapter.port()))
			adapter.close(t)
			if code != test.wantCode || stderr == "" {
				t.Fatalf("code=%d stderr=%q", code, stderr)
			}
			var output map[string]any
			if err := json.Unmarshal([]byte(strings.TrimSpace(stdout)), &output); err != nil {
				t.Fatalf("stdout=%q err=%v", stdout, err)
			}
			if output["result"] != (test.wantCode == 0) {
				t.Fatalf("output=%v", output)
			}
		})
	}
}

func TestCLIRunScenarioSummaryAndExitCodes(t *testing.T) {
	for _, test := range []struct {
		name     string
		value    float64
		wantCode int
	}{
		{"pass", 0, 0}, {"assertion failure", -9, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			adapter := startCLIAdapter(t)
			path := t.TempDir() + "/scenario.json"
			scenario := fmt.Sprintf(`{"name":"cli","steps":[{"cmd":"assert","name":"z","get":["hmd","head","position",2],"op":"eq","value":%g}]}`, test.value)
			if err := os.WriteFile(path, []byte(scenario), 0o600); err != nil {
				t.Fatal(err)
			}
			code, stdout, stderr := captureRun(t, "run", path, "--port", strconv.Itoa(adapter.port()))
			adapter.close(t)
			if code != test.wantCode || !strings.Contains(stderr, "step: assert") {
				t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
			}
			var summary map[string]any
			if err := json.Unmarshal([]byte(strings.TrimSpace(stdout)), &summary); err != nil {
				t.Fatal(err)
			}
			if summary["ok"] != (test.wantCode == 0) || summary["asserts"] != float64(1) {
				t.Fatalf("summary=%v", summary)
			}
		})
	}

	bad := t.TempDir() + "/bad.json"
	_ = os.WriteFile(bad, []byte("{"), 0o600)
	code, stdout, stderr := captureRun(t, "run", bad)
	if code != 2 || stdout != "" || !strings.Contains(stderr, "decode scenario") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestCLIConnectionFailureUsesExitTwoWithoutJSON(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	code, stdout, stderr := captureRun(t, "cmd", "get-state", "--port", strconv.Itoa(port))
	if code != 2 || stdout != "" || stderr == "" {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestCLIBadArgsJSONUsesExitTwoWithoutConnecting(t *testing.T) {
	code, stdout, stderr := captureRun(t, "cmd", "look", "--args", "{")
	if code != 2 || stdout != "" || !strings.Contains(stderr, "bad --args JSON") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestArgsJSONSilentlyDiscardsTypedFlags(t *testing.T) {
	// Characterization of the current behaviour, not a recommendation: --args
	// replaces the entire argument object, so a typed flag passed alongside it
	// is discarded without a warning and the command still exits 0. The pair of
	// runs is the point -- the same --z reaches the device in the second run,
	// which is what makes the first one a silent loss rather than a no-op.
	for _, test := range []struct {
		name  string
		args  []string
		wantZ float64
	}{
		{"typed flag alongside --args", []string{"move-head", "--args", `{"duration_ms":0}`, "--z", "-7"}, 0},
		{"typed flag alone", []string{"move-head", "--z", "-7", "--duration-ms", "0"}, -7},
	} {
		t.Run(test.name, func(t *testing.T) {
			adapter := startCLIAdapter(t)
			args := append([]string{"cmd"}, test.args...)
			code, stdout, stderr := captureRun(t, append(args, "--port", strconv.Itoa(adapter.port()))...)
			adapter.close(t)
			if code != 0 {
				t.Fatalf("code=%d stderr=%q", code, stderr)
			}
			var output map[string]any
			if err := json.Unmarshal([]byte(strings.TrimSpace(stdout)), &output); err != nil {
				t.Fatal(err)
			}
			if got := core.Resolve(output["state"], []any{"hmd", "head", "position", 2}); got != test.wantZ {
				t.Fatalf("head z = %v, want %v", got, test.wantZ)
			}
		})
	}
}

func TestScenarioArgumentSplitDropsSingleDashFlags(t *testing.T) {
	// run/record/replay separate the file path from the flags themselves, and
	// that splitter only recognises "--". Characterization of what that costs:
	// a single-dash flag is dropped, so the command silently keeps the default
	// :52702 and talks to whatever is listening there instead of the requested
	// target; when the flag comes first, its name is taken as the file path.
	for _, test := range []struct {
		name      string
		args      []string
		wantPath  string
		wantFlags []string
	}{
		{"double dash is kept", []string{"scenario.json", "--port", "52999"}, "scenario.json", []string{"--port", "52999"}},
		{"single dash is dropped", []string{"scenario.json", "-port", "52999"}, "scenario.json", []string{}},
		{"leading single dash becomes the path", []string{"-port", "52999", "scenario.json"}, "-port", []string{}},
		{"second positional is dropped", []string{"a.json", "b.json"}, "a.json", []string{}},
	} {
		t.Run(test.name, func(t *testing.T) {
			path, flags := firstPathAndFlags(test.args)
			if path != test.wantPath || !reflect.DeepEqual(flags, test.wantFlags) {
				t.Fatalf("path=%q flags=%v, want path=%q flags=%v", path, flags, test.wantPath, test.wantFlags)
			}
		})
	}

	// The leading-flag form is observable end to end without a network: the
	// scenario path becomes "-port", so the run dies on the file read.
	code, stdout, stderr := captureRun(t, "run", "-port", "52999", "scenario.json")
	if code != 2 || stdout != "" || !strings.Contains(stderr, "-port") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestScenarioWithoutAssertionsExitsZero(t *testing.T) {
	// A scenario that asserts nothing is reported as a pass: {"asserts":0,
	// "ok":true} with exit 0. That is the documented contract, and the cost of
	// it is that a misspelled "Steps" key produces a run indistinguishable from
	// a successful one -- pinned here so the contract is a decision on record.
	adapter := startCLIAdapter(t)
	path := t.TempDir() + "/typo.json"
	scenario := `{"name":"typo","Steps":[{"cmd":"assert","get":["hmd","head","position",2],"op":"eq","value":-9}]}`
	if err := os.WriteFile(path, []byte(scenario), 0o600); err != nil {
		t.Fatal(err)
	}
	code, stdout, stderr := captureRun(t, "run", path, "--port", strconv.Itoa(adapter.port()))
	adapter.close(t)
	if code != 0 {
		t.Fatalf("code=%d stderr=%q", code, stderr)
	}
	var summary map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(stdout)), &summary); err != nil {
		t.Fatal(err)
	}
	if summary["asserts"] != float64(0) || summary["ok"] != true || summary["failed"] != float64(0) {
		t.Fatalf("summary=%v", summary)
	}
}

func TestDoctorReportsBothChannelsButExitsOnOperateAlone(t *testing.T) {
	// doctor is the liveness signal the setup scripts read, so both halves
	// matter: the JSON has to describe capture as well, and the exit code has to
	// come from the operate channel alone -- a capture port that is down must
	// not fail the check, and an operate port that is down must.
	open, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer open.Close()
	reachable := open.Addr().(*net.TCPAddr).Port
	spare, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	unreachable := spare.Addr().(*net.TCPAddr).Port
	_ = spare.Close()

	for _, test := range []struct {
		name             string
		operate, capture int
		wantCode         int
	}{
		{"capture down does not fail doctor", reachable, unreachable, 0},
		{"operate down fails doctor", unreachable, reachable, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			code, stdout, stderr := captureRun(t, "doctor", "--port", strconv.Itoa(test.operate), "--capture-port", strconv.Itoa(test.capture))
			if code != test.wantCode || stderr != "" {
				t.Fatalf("code=%d, want %d; stderr=%q", code, test.wantCode, stderr)
			}
			var report map[string]any
			if err := json.Unmarshal([]byte(strings.TrimSpace(stdout)), &report); err != nil {
				t.Fatalf("stdout=%q: %v", stdout, err)
			}
			if report["version"] != version {
				t.Fatalf("version=%v", report["version"])
			}
			for channel, port := range map[string]int{"operate": test.operate, "capture": test.capture} {
				probed, ok := report[channel].(map[string]any)
				if !ok || probed["host"] != "127.0.0.1" || probed["port"] != float64(port) {
					t.Fatalf("%s = %v", channel, report[channel])
				}
				if probed["reachable"] != (port == reachable) {
					t.Fatalf("%s reachable = %v", channel, probed["reachable"])
				}
				if diagnosis, _ := probed["error"].(string); port != reachable && diagnosis == "" {
					t.Fatalf("%s carries no diagnosis: %v", channel, probed)
				}
			}
		})
	}
}

func TestLegacyDashDashCmdMatchesTheCmdSubcommand(t *testing.T) {
	// --cmd is the old single-command CLI's entry point and scripts still use
	// it, so it has to produce the same stdout as the subcommand, not merely
	// work.
	outputs := map[string]string{}
	for _, entry := range [][]string{{"cmd", "get-state"}, {"--cmd", "get-state"}} {
		adapter := startCLIAdapter(t)
		code, stdout, stderr := captureRun(t, append(append([]string{}, entry...), "--port", strconv.Itoa(adapter.port()))...)
		adapter.close(t)
		if code != 0 || stderr != "" {
			t.Fatalf("%v: code=%d stderr=%q", entry, code, stderr)
		}
		outputs[entry[0]] = stdout
	}
	if outputs["cmd"] != outputs["--cmd"] {
		t.Fatalf("--cmd stdout:\n%s\ncmd stdout:\n%s", outputs["--cmd"], outputs["cmd"])
	}
	code, stdout, stderr := captureRun(t, "--cmd")
	if code != 2 || stdout != "" || !strings.Contains(stderr, "--cmd requires an operation") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestImageStatsFailsOnTruncatedPNG(t *testing.T) {
	// A capture read while it is still being written is a real, recurring
	// failure, and the only signal a script gets is the exit code: the existing
	// coverage only walks paths that exit 0 (missing file, non-PNG), so a
	// regression that swallowed decode errors would stay green. The truncation
	// cuts into the IDAT payload rather than between chunks -- stopping short of
	// a chunk header instead yields a well-formed header with no pixel data,
	// which is reported as an unsupported encoding and still exits 0.
	full := encodedPNG(t, 64, 64)
	marker := bytes.Index(full, []byte("IDAT"))
	if marker < 0 {
		t.Fatal("fixture has no IDAT chunk")
	}
	directory := t.TempDir()
	good, truncated := directory+"/good.png", directory+"/truncated.png"
	if err := os.WriteFile(good, full, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(truncated, full[:marker+8], 0o600); err != nil {
		t.Fatal(err)
	}

	code, stdout, stderr := captureRun(t, "image-stats", good)
	if code != 0 || stderr != "" || !strings.Contains(stdout, "non-degenerate: True") {
		t.Fatalf("intact PNG: code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
	code, stdout, stderr = captureRun(t, "image-stats", good, truncated)
	if code != 1 {
		t.Fatalf("truncated PNG: code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stderr, truncated) || !strings.Contains(stdout, good) {
		t.Fatalf("stdout=%q stderr=%q", stdout, stderr)
	}
}

func encodedPNG(t *testing.T, width, height int) []byte {
	t.Helper()
	canvas := image.NewRGBA(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			canvas.Set(x, y, color.RGBA{R: uint8(x * 3), G: uint8(y * 5), B: uint8(x ^ y), A: 255})
		}
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, canvas); err != nil {
		t.Fatal(err)
	}
	return encoded.Bytes()
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

// TestInternalExtractMonadoFailsWhenNothingWasExtracted pins the exit code the
// setup scripts read. An archive whose layout no longer puts anything under
// install/ used to print "extracted 0 files" and exit 0, so the script carried
// on to launch a runtime that had never been unpacked.
func TestInternalExtractMonadoFailsWhenNothingWasExtracted(t *testing.T) {
	zipPath := t.TempDir() + "/monado.zip"
	file, err := os.Create(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	archive := zip.NewWriter(file)
	writer, err := archive.Create("build/bin/monado-service.exe")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := writer.Write([]byte("moved out of install/")); err != nil {
		t.Fatal(err)
	}
	if err := archive.Close(); err != nil {
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}

	destination := t.TempDir() + "/monado"
	code, stdout, stderr := captureRun(t, "internal", "extract-monado", "--zip", zipPath, "--destination", destination)
	if code != 1 || stdout != "" || !strings.Contains(stderr, "0 files") {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
}

// TestVerifyCouplingFlagsCarryThePythonProbeConstants pins where the coupling
// probe's constants live. verify.Coupling used to substitute its own defaults
// for a zero target or tolerance, which silently rewrote a caller asking for the
// origin or for an exact match; now the CLI flags are the only place the Python
// probe's TARGET_Z and TOL survive, so losing them here would change what the
// command measures without failing anything else.
func TestVerifyCouplingFlagsCarryThePythonProbeConstants(t *testing.T) {
	code, stdout, stderr := captureRun(t, "verify", "coupling", "--not-a-flag")
	if code != 2 || stdout != "" {
		t.Fatalf("code=%d stdout=%q stderr=%q", code, stdout, stderr)
	}
	for _, want := range []string{"-target-z float", "(default -2.5)", "-tolerance float", "(default 0.3)"} {
		if !strings.Contains(stderr, want) {
			t.Errorf("coupling usage is missing %q: %s", want, stderr)
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
