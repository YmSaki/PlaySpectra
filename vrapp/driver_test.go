package vrapp

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestStageGlobalCoordinateConversion(t *testing.T) {
	origin := Event{"pos": []any{10.0, -2.0, 3.0}}
	stage := []float64{1, 2, 3}
	global := StageToGlobal(stage, origin)
	if want := []float64{11, 0, 6}; !reflect.DeepEqual(global, want) {
		t.Fatalf("global = %v, want %v", global, want)
	}
	if got := GlobalToStage(global, origin); !reflect.DeepEqual(got, stage) {
		t.Fatalf("stage = %v, want %v", got, stage)
	}
	if !reflect.DeepEqual(stage, []float64{1, 2, 3}) {
		t.Fatalf("input was mutated: %v", stage)
	}
}

func TestLineParserAndPredicates(t *testing.T) {
	app := New(Config{})
	app.consumeLine("engine startup")
	app.consumeLine("[VRTEST] not-json")
	app.consumeLine(`[VRTEST] {"t":"axis","hand":"left","name":"trigger","value":0.7}`)
	app.consumeLine(`[VRTEST] {"t":"button","hand":"right","state":"pressed"}`)
	app.consumeLine(`[VRTEST] {"t":"testbed","event":"hover_start","target":"Cube","source":"left"}`)
	if app.Count() != 3 {
		t.Fatalf("events = %d", app.Count())
	}
	if lines := app.NonVRTestLines(25); len(lines) != 1 || lines[0] != "engine startup" {
		t.Fatalf("non-VRTEST lines = %v", lines)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if Axis("left", "trigger")(app.WaitFor(ctx, Axis("left", "trigger"), 0)) != true {
		t.Fatal("axis predicate did not match")
	}
	if Button("right", "pressed")(app.WaitFor(ctx, Button("right", "pressed"), 0)) != true {
		t.Fatal("button predicate did not match")
	}
	if HoverStart("Cube", "left")(app.WaitFor(ctx, HoverStart("Cube", "left"), 0)) != true {
		t.Fatal("hover predicate did not match")
	}
}

func TestWaitForHonorsSinceAndContext(t *testing.T) {
	app := New(Config{})
	app.done = make(chan struct{})
	app.consumeLine(`[VRTEST] {"t":"axis","hand":"left","name":"trigger"}`)
	ctx, cancel := context.WithTimeout(context.Background(), 80*time.Millisecond)
	defer cancel()
	if event := app.WaitFor(ctx, Axis("left", "trigger"), app.Count()); event != nil {
		t.Fatalf("old event satisfied wait: %v", event)
	}
}

// TestWaitForGivesUpWhenTheProcessIsGone exercises the one branch that keeps a
// crashed app from costing the whole suite its timeouts. Every wait in the
// suite is bounded at 8 to 45 seconds, so without the done branch a run that
// dies during startup would sit through all of them in turn instead of failing
// at once. The existing wait coverage never closes done, so it only ever proves
// the context branch.
func TestWaitForGivesUpWhenTheProcessIsGone(t *testing.T) {
	app := New(Config{})
	app.consumeLine(`[VRTEST] {"t":"axis","hand":"left","name":"trigger"}`)
	app.done = make(chan struct{})
	close(app.done)

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	started := time.Now()
	if event := app.WaitFor(ctx, Axis("right", "trigger"), 0); event != nil {
		t.Fatalf("a predicate that matches nothing returned %v", event)
	}
	if elapsed := time.Since(started); elapsed > time.Second {
		t.Fatalf("waited %s for an event from a dead process", elapsed)
	}

	// Events already buffered before the process died are still delivered; the
	// exit ends the waiting, not the reading.
	if event := app.WaitFor(ctx, Axis("left", "trigger"), 0); event == nil {
		t.Fatal("a buffered event was dropped because the process had exited")
	}
}

// TestStartRejectsASecondLaunchButStaysRetryable pins the guard that keeps a
// second Start from overwriting the first child's handle, which would strand a
// GUI process with nothing left to kill it. A start that failed must not latch
// that way, because the caller's next move is to retry.
func TestStartRejectsASecondLaunchButStaysRetryable(t *testing.T) {
	if os.Getenv("GO_WANT_VRAPP_HELPER") == "1" {
		vrappHelperProcess()
		return
	}
	app := New(Config{
		Executable: os.Args[0],
		Arguments:  []string{"-test.run=TestStartRejectsASecondLaunchButStaysRetryable", "--"},
		Env:        append(os.Environ(), "GO_WANT_VRAPP_HELPER=1"),
	})
	if err := app.Start(); err != nil {
		t.Fatal(err)
	}
	defer app.Stop()
	err := app.Start()
	if err == nil {
		t.Fatal("a second Start replaced the running child's handle")
	}
	if err.Error() != "VRApp is already started" {
		t.Fatalf("second start error = %v", err)
	}

	absent := New(Config{Executable: filepath.Join(t.TempDir(), "absent.exe")})
	first := absent.Start()
	if first == nil {
		t.Fatal("starting a missing executable succeeded")
	}
	second := absent.Start()
	if second == nil {
		t.Fatal("a failed start left the app latched as running")
	}
	if strings.Contains(second.Error(), "already started") {
		t.Fatalf("a failed start blocks the retry: %v", second)
	}
}

// TestTranslateWithoutAUsableOriginPassesThrough records what happens when the
// app's pose snapshot has no origin: STAGE and GLOBAL coordinates are treated
// as the same frame. Every interaction target in the suite is a GLOBAL position
// converted through this, so a runtime that stopped reporting an origin would
// silently reach for the wrong place rather than fail. The conversion is left
// as it is -- the suite, not this function, is where a missing origin should be
// noticed.
func TestTranslateWithoutAUsableOriginPassesThrough(t *testing.T) {
	tests := []struct {
		name   string
		origin Event
		want   []float64
	}{
		{"no origin at all", Event{}, []float64{1, 2, 3}},
		{"an origin without a position", Event{"rot": []any{0.0, 0.0, 0.0, 1.0}}, []float64{1, 2, 3}},
		{"a position that is not a vector", Event{"pos": "0,0,0"}, []float64{1, 2, 3}},
		{"a short position translates what it covers", Event{"pos": []any{10.0, 10.0}}, []float64{11, 12, 3}},
		// A non-numeric member is skipped rather than held open, so the axes
		// after it shift down one: z's offset lands on y.
		{"a non-numeric member shifts the axes after it", Event{"pos": []any{10.0, "up", 20.0}}, []float64{11, 22, 3}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := StageToGlobal([]float64{1, 2, 3}, test.origin); !reflect.DeepEqual(got, test.want) {
				t.Fatalf("StageToGlobal = %v, want %v", got, test.want)
			}
		})
	}
}

func TestAppProcessRequestLifecycleAndLog(t *testing.T) {
	if os.Getenv("GO_WANT_VRAPP_HELPER") == "1" {
		vrappHelperProcess()
		return
	}
	logPath := t.TempDir() + "/vrapp.log"
	environment := append(os.Environ(), "GO_WANT_VRAPP_HELPER=1")
	app := New(Config{Executable: os.Args[0], Arguments: []string{"-test.run=TestAppProcessRequestLifecycleAndLog", "--"}, Env: environment, LogPath: logPath})
	if err := app.Start(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if init := app.WaitXRInit(ctx); init == nil || init["ok"] != true {
		t.Fatalf("xr_init = %v", init)
	}
	if active := app.WaitControllerActive(ctx, "left"); active == nil || active["profile"] != "fixture" {
		t.Fatalf("controller = %v", active)
	}
	pose := app.Pose(ctx)
	if pose == nil || pose["t"] != "pose_snapshot" || integer(pose["id"]) != 1 {
		t.Fatalf("pose = %v", pose)
	}
	if err := app.Stop(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(logPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), "engine fixture") || !strings.Contains(string(data), "pose_snapshot") {
		t.Fatalf("log = %q", data)
	}
}

func vrappHelperProcess() {
	fmt.Println("engine fixture")
	fmt.Println(`[VRTEST] {"t":"xr_init","ok":true}`)
	fmt.Println(`[VRTEST] {"t":"controller_state","hand":"left","active":true,"profile":"fixture"}`)
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		var request map[string]any
		if json.Unmarshal(scanner.Bytes(), &request) != nil {
			continue
		}
		fmt.Printf("[VRTEST] {\"t\":\"pose_snapshot\",\"id\":%v,\"origin\":{\"pos\":[0,0,0]}}\n", request["id"])
	}
	os.Exit(0)
}
