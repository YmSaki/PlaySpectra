// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"net"
	"path/filepath"
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

// TestCompatibilityCoverageIsIndependentOfTheReportResult separates the two
// questions a probe answers: did its checks pass, and did it run the checks the
// Python probe ran. A probe that quietly stopped adding checks still reports
// OK() and exit 0 for whatever is left -- an empty report passes vacuously --
// so this guard is the only thing between a dropped check and a green run. The
// existing coverage only ever asserts the nil side of it, which a deleted guard
// would satisfy just as well.
func TestCompatibilityCoverageIsIndependentOfTheReportResult(t *testing.T) {
	empty := NewReport("reset")
	if !empty.OK() || empty.ExitCode() != 0 {
		t.Fatalf("an empty report no longer looks green, so the guard is not the only signal: %+v", empty)
	}
	for _, test := range []struct {
		name  string
		count int
		want  []string
	}{
		{"reset", 0, []string{"reset", "got 0", "want 20"}},
		{"multi-observer", 10, []string{"multi-observer", "got 10", "want 11"}},
		{"mcp", 15, []string{"mcp", "got 15", "want 14"}},
	} {
		report := NewReport(test.name)
		for range test.count {
			report.Add("fixture", true, nil)
		}
		err := report.ValidateCompatibilityCoverage()
		if err == nil {
			t.Errorf("%s with %d checks passed the coverage guard", test.name, test.count)
			continue
		}
		for _, want := range test.want {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("%s coverage error is missing %q: %v", test.name, want, err)
			}
		}
	}

	// A probe with no Python ancestor has no inventory to be measured against,
	// so it is unconstrained rather than expected to have zero checks.
	untracked := NewReport("experimental")
	untracked.Add("fixture", false, nil)
	if err := untracked.ValidateCompatibilityCoverage(); err != nil {
		t.Errorf("untracked report was held to an inventory: %v", err)
	}
	if err := NewReport("experimental").ValidateCompatibilityCoverage(); err != nil {
		t.Errorf("empty untracked report was held to an inventory: %v", err)
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

// TestCompatibilityCheckInventoryMatchesProbeSources works on two different
// counts of the same probes, so the numbers below deliberately disagree:
//
//   - wantAddCalls counts report.Add call sites in the source. A call site
//     inside a loop still counts once, which is why MultiObserver is 9 (one
//     site broadcasts to three clients) and MCP is 13 (one site covers
//     walk_forward and strafe).
//   - the second table drives ValidateCompatibilityCoverage, whose expectations
//     in compatibilityCheckCounts are checks after the loops run: 11 and 14.
//
// The source count catches a whole check being deleted; the runtime count
// catches a loop losing an iteration. Both matter, and probe_counts_test.go
// produces the runtime counts from real probe runs rather than from literals.
func TestCompatibilityCheckInventoryMatchesProbeSources(t *testing.T) {
	wantAddCalls := map[string]int{
		"Server": 9, "Recording": 5, "FrameSynchronized": 10, "Reset": 20,
		"MultiObserver": 9, "Coupling": 2, "MCP": 13,
	}
	files, err := filepath.Glob("*.go")
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]int{}
	set := token.NewFileSet()
	for _, path := range files {
		if strings.HasSuffix(path, "_test.go") {
			continue
		}
		file, err := parser.ParseFile(set, path, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		for _, declaration := range file.Decls {
			function, ok := declaration.(*ast.FuncDecl)
			if !ok || function.Body == nil {
				continue
			}
			if _, tracked := wantAddCalls[function.Name.Name]; !tracked {
				continue
			}
			ast.Inspect(function.Body, func(node ast.Node) bool {
				call, ok := node.(*ast.CallExpr)
				if !ok {
					return true
				}
				selector, ok := call.Fun.(*ast.SelectorExpr)
				if !ok {
					return true
				}
				receiver, isIdentifier := selector.X.(*ast.Ident)
				if isIdentifier && receiver.Name == "report" && selector.Sel.Name == "Add" {
					got[function.Name.Name]++
				}
				return true
			})
		}
	}
	for function, want := range wantAddCalls {
		if got[function] != want {
			t.Errorf("%s has %d report.Add calls, want %d from the Python compatibility inventory", function, got[function], want)
		}
	}
	// Post-loop check counts, keyed by report name rather than function name.
	for name, want := range map[string]int{
		"server": 9, "record-replay": 5, "frame-synchronized": 10, "reset": 20,
		"multi-observer": 11, "runtime-coupling": 2, "mcp": 14,
	} {
		report := NewReport(name)
		for range want {
			report.Add("fixture", true, nil)
		}
		if err := report.ValidateCompatibilityCoverage(); err != nil {
			t.Errorf("%s: %v", name, err)
		}
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
