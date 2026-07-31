package vrapp

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"os"
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
