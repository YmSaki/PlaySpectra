package vrapp

import (
	"bytes"
	"context"
	"os"
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
