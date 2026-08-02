package verify

import (
	"context"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

// assertProbeReport pins a probe to the compatibility inventory by what it does
// rather than by how it is written: the count comes from running the probe
// against a fixture, so a check that vanishes inside a loop -- one element
// dropped from the multi-observer event client list, say -- is caught even
// though the source still contains the same number of report.Add calls.
// Requiring every check to pass keeps the count from being satisfied by a probe
// that reports the right number of failures.
func assertProbeReport(t *testing.T, report Report, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("%s probe: %v", report.Name, err)
	}
	want, tracked := compatibilityCheckCounts[report.Name]
	if !tracked {
		t.Fatalf("probe %q is missing from compatibilityCheckCounts", report.Name)
	}
	if len(report.Checks) != want {
		t.Fatalf("%s ran %d checks, want %d from the Python compatibility inventory", report.Name, len(report.Checks), want)
	}
	if err := report.ValidateCompatibilityCoverage(); err != nil {
		t.Fatal(err)
	}
	for _, check := range report.Checks {
		if !check.OK {
			t.Errorf("%s: %q failed: %v", report.Name, check.Name, check.Detail)
		}
	}
}

func TestServerProbeRunsItsNineChecks(t *testing.T) {
	adapter := startFixtureAdapter(t)
	report, err := Server(context.Background(), adapter.address(), 60)
	assertProbeReport(t, report, err)
}

func TestRecordingProbeRunsItsFiveChecks(t *testing.T) {
	adapter := startFixtureAdapter(t)
	report, err := Recording(context.Background(), adapter.address(), 60)
	assertProbeReport(t, report, err)
}

func TestResetProbeRunsItsTwentyChecks(t *testing.T) {
	adapter := startFixtureAdapter(t)
	report, err := Reset(context.Background(), adapter.address())
	assertProbeReport(t, report, err)
}

func TestMultiObserverProbeRunsItsElevenChecks(t *testing.T) {
	adapter := startFixtureAdapter(t)
	report, err := MultiObserver(context.Background(), adapter.address(), 3*time.Second)
	assertProbeReport(t, report, err)
}

func TestCouplingProbeRunsItsTwoChecks(t *testing.T) {
	adapter := startFixtureAdapter(t)
	capture := startFixtureCapture(t, adapter)
	report, err := Coupling(context.Background(), adapter.address(), capture.address(), -2.5, 0.3)
	assertProbeReport(t, report, err)
}

func TestMCPProbeRunsItsFourteenChecks(t *testing.T) {
	executable := buildControlPlane(t)
	adapter := startFixtureAdapter(t)
	capture := startFixtureCapture(t, adapter)
	report, err := MCP(context.Background(), executable, "127.0.0.1", adapter.port(), capture.port())
	assertProbeReport(t, report, err)
}

// buildControlPlane compiles the binary the MCP probe spawns. The probe drives
// the frontend over stdio as a child process, so there is no in-process route
// to it.
func buildControlPlane(t *testing.T) string {
	t.Helper()
	if _, err := exec.LookPath("go"); err != nil {
		t.Skip("SKIP: no go toolchain on PATH, cannot build the control plane the MCP probe spawns")
	}
	executable := filepath.Join(t.TempDir(), "playspectra")
	if runtime.GOOS == "windows" {
		executable += ".exe"
	}
	build := exec.Command("go", "build", "-o", executable, "./cmd/playspectra")
	build.Dir = ".."
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("go build ./cmd/playspectra: %v\n%s", err, output)
	}
	return executable
}
