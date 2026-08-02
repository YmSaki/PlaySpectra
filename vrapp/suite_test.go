// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package vrapp

import (
	"bytes"
	"context"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSuiteReportCountsFailuresAndSkips(t *testing.T) {
	var output bytes.Buffer
	suite := &Suite{Output: &output}
	suite.Check("pass", true, "ok")
	suite.Check("fail", false, "bad")
	suite.Skip("optional", "missing")
	if code := suite.Report(); code != 1 || suite.Passed != 1 || suite.Failed != 1 || len(suite.Skipped) != 1 {
		t.Fatalf("code=%d suite=%+v", code, suite)
	}
	if !strings.Contains(output.String(), "1/2 passed, 1 skipped") {
		t.Fatalf("output = %q", output.String())
	}
}

func TestRunSuiteSkipsMissingApplication(t *testing.T) {
	var output bytes.Buffer
	code := RunSuite(context.Background(), SuiteConfig{Executable: t.TempDir() + "/missing.exe", Output: &output})
	if code != 0 || !strings.Contains(output.String(), "SKIP: VRAppDummyGame not built") {
		t.Fatalf("code=%d output=%q", code, output.String())
	}
}

// TestRunSuiteFailsOnAnUnusableExecutable draws the line the CI signal depends
// on: "not built yet" is a skip, but "built and broken" is a failure. Both look
// the same to os.Stat -- a directory and a zero-byte file both exist -- so the
// distinction only survives if the launch failure is reported rather than
// swallowed. A regression here turns every broken build into a green run.
func TestRunSuiteFailsOnAnUnusableExecutable(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	port := listener.Addr().(*net.TCPAddr).Port

	directory := filepath.Join(t.TempDir(), "vrapp.console.exe")
	if err := os.Mkdir(directory, 0o755); err != nil {
		t.Fatal(err)
	}
	empty := filepath.Join(t.TempDir(), "vrapp.console.exe")
	if err := os.WriteFile(empty, nil, 0o600); err != nil {
		t.Fatal(err)
	}

	for name, executable := range map[string]string{
		"a directory named like the executable": directory,
		"a zero-byte executable":                empty,
	} {
		t.Run(name, func(t *testing.T) {
			var output bytes.Buffer
			code := RunSuite(context.Background(), SuiteConfig{
				Executable: executable, OperateHost: "127.0.0.1", OperatePort: port, Output: &output,
			})
			if code != 1 {
				t.Fatalf("code=%d, want 1\n%s", code, output.String())
			}
			if !strings.Contains(output.String(), "FAIL [startup] VRApp process starts") {
				t.Errorf("the launch failure was not reported: %s", output.String())
			}
			if strings.Contains(output.String(), "SKIP") {
				t.Errorf("a broken build was downgraded to a skip: %s", output.String())
			}
		})
	}
}

// TestNestedNumberRequiresANumber guards the type check that keeps a changed
// status payload from being read as a measurement. Without it a missing or
// re-typed framesObserved would come back as 0 and the pacing check would
// report a performance regression instead of skipping.
func TestNestedNumberRequiresANumber(t *testing.T) {
	status := map[string]any{
		"capture": map[string]any{
			"framesObserved": 1200.0,
			"integerCount":   7,
			"asText":         "1200",
			"nested":         map[string]any{"deep": 3.5},
		},
		"notAMap": "capture",
	}
	tests := []struct {
		name  string
		keys  []string
		value float64
		ok    bool
	}{
		{"a float reads through", []string{"capture", "framesObserved"}, 1200, true},
		{"an int reads through", []string{"capture", "integerCount"}, 7, true},
		{"a nested float reads through", []string{"capture", "nested", "deep"}, 3.5, true},
		{"a numeric string is not a number", []string{"capture", "asText"}, 0, false},
		{"a missing leaf", []string{"capture", "absent"}, 0, false},
		{"a missing branch", []string{"absent", "framesObserved"}, 0, false},
		{"a branch that is not a map", []string{"notAMap", "framesObserved"}, 0, false},
		{"a map is not a number", []string{"capture"}, 0, false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			value, ok := nestedNumber(status, test.keys...)
			if value != test.value || ok != test.ok {
				t.Fatalf("nestedNumber(%v) = (%v, %v), want (%v, %v)", test.keys, value, ok, test.value, test.ok)
			}
		})
	}
}

func TestRunSuiteSkipsMissingOperateChannel(t *testing.T) {
	executable := t.TempDir() + "/vrapp.console.exe"
	if err := os.WriteFile(executable, []byte("fixture"), 0o600); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	code := RunSuite(context.Background(), SuiteConfig{Executable: executable, OperateHost: "127.0.0.1", OperatePort: 1, Output: &output})
	if code != 0 || !strings.Contains(output.String(), "SKIP: no operate channel") {
		t.Fatalf("code=%d output=%q", code, output.String())
	}
}
