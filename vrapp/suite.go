// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package vrapp

import (
	"context"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/YmSaki/PlaySpectra/playspectra"
	"github.com/YmSaki/PlaySpectra/pngstats"
	"github.com/YmSaki/PlaySpectra/protocol"
)

const (
	positionTolerance = 0.05
	valueTolerance    = 0.05
)

var (
	inspectorPosition = []float64{0, 1.6, 4.534}
	pushButton        = []float64{3, 1.05, 3}
	grabCubeResting   = []float64{-3, 0.075, 3}
	leverHandle       = []float64{-3, 1.28, -3}
)

type SuiteConfig struct {
	Executable  string
	OperateHost string
	OperatePort int
	CapturePort int
	LogPath     string
	Output      io.Writer
}

type Suite struct {
	Passed  int
	Failed  int
	Skipped []string
	Output  io.Writer
}

func (s *Suite) Check(name string, condition bool, detail any) bool {
	if condition {
		s.Passed++
	} else {
		s.Failed++
	}
	suffix := ""
	if detail != nil && fmt.Sprint(detail) != "" {
		suffix = "  - " + fmt.Sprint(detail)
	}
	fmt.Fprintf(s.Output, "%s %s%s\n", map[bool]string{true: "PASS", false: "FAIL"}[condition], name, suffix)
	return condition
}

func (s *Suite) Skip(name, reason string) {
	s.Skipped = append(s.Skipped, name)
	fmt.Fprintf(s.Output, "SKIP %s  - %s\n", name, reason)
}

func (s *Suite) Report() int {
	total := s.Passed + s.Failed
	fmt.Fprintf(s.Output, "=== VRAppDummyGame E2E %d/%d passed", s.Passed, total)
	if len(s.Skipped) > 0 {
		fmt.Fprintf(s.Output, ", %d skipped (%s)", len(s.Skipped), strings.Join(s.Skipped, ", "))
	}
	fmt.Fprintln(s.Output, " ===")
	if s.Failed > 0 {
		return 1
	}
	return 0
}

func RunSuite(ctx context.Context, config SuiteConfig) int {
	if config.Output == nil {
		config.Output = os.Stdout
	}
	if config.Executable == "" {
		config.Executable = DefaultExecutable()
	}
	if config.OperateHost == "" {
		config.OperateHost = "127.0.0.1"
	}
	if config.OperatePort == 0 {
		config.OperatePort = envPort("PLAYSPECTRA_MONADO_PORT", 52702)
	}
	if config.CapturePort == 0 {
		config.CapturePort = envPort("PLAYSPECTRA_PORT", 52700)
	}
	if config.LogPath == "" {
		config.LogPath = filepath.Join(os.TempDir(), "playspectra_vrapp_e2e.log")
	}
	if _, err := os.Stat(config.Executable); err != nil {
		fmt.Fprintf(config.Output, "SKIP: VRAppDummyGame not built: %s\n", config.Executable)
		fmt.Fprintln(config.Output, "      export it with Godot 4.7 (--export-release \"Windows Desktop\" build/vrapp.exe) or set PLAYSPECTRA_VRAPP_EXE")
		return 0
	}
	if !PortOpen(config.OperateHost, config.OperatePort, time.Second) {
		fmt.Fprintf(config.Output, "SKIP: no operate channel on :%d (start monado-service with PLAYSPECTRA_ENABLE=1)\n", config.OperatePort)
		return 0
	}
	fmt.Fprintf(config.Output, "app      : %s\n", config.Executable)
	revision := AppRevision(config.Executable)
	if revision == "" {
		revision = "(unknown)"
	}
	fmt.Fprintf(config.Output, "app rev  : %s\n", revision)
	fmt.Fprintf(config.Output, "operate  : :%d   capture: :%d\n", config.OperatePort, config.CapturePort)

	suite := &Suite{Output: config.Output}
	app := New(Config{Executable: config.Executable, LogPath: config.LogPath})
	if err := app.Start(); err != nil {
		suite.Check("[startup] VRApp process starts", false, err)
		return suite.Report()
	}
	defer app.Stop()
	if !phaseStartup(ctx, app, suite) {
		fmt.Fprintf(config.Output, "app log: %s\n", config.LogPath)
		return suite.Report()
	}

	operate, err := protocol.Dial(ctx, net.JoinHostPort(config.OperateHost, strconv.Itoa(config.OperatePort)))
	if err != nil {
		suite.Check("[startup] operate channel connects", false, err)
		return suite.Report()
	}
	defer operate.Close()
	var capture *protocol.Client
	if WaitPort(ctx, config.OperateHost, config.CapturePort, 20*time.Second) {
		capture, _ = protocol.Dial(ctx, net.JoinHostPort(config.OperateHost, strconv.Itoa(config.CapturePort)))
		if capture != nil {
			defer capture.Close()
		}
	}
	options := []playspectra.ServerOption{playspectra.WithLogger(func(string, ...any) {})}
	if capture != nil {
		options = append(options, playspectra.WithCapture(capture))
	}
	server := playspectra.NewServer(operate, options...)
	if err := server.Hello(ctx, "vrapp-e2e"); err != nil {
		suite.Check("[startup] writer hello", false, err)
		return suite.Report()
	}
	sleepContext(ctx, time.Second)
	phasePoseAndInputs(ctx, app, server, suite)
	if capture == nil {
		suite.Skip("[capture] all", fmt.Sprintf("no layer control channel on :%d (run with the layer enabled)", config.CapturePort))
		suite.Skip("[pacing] observed frame rate", "needs the layer's framesObserved counter")
	} else {
		phaseCapture(ctx, server, suite)
		phasePacing(ctx, capture, suite)
	}
	phaseInteractions(ctx, app, server, suite)
	fmt.Fprintf(config.Output, "app log: %s\n", config.LogPath)
	return suite.Report()
}

func phaseStartup(parent context.Context, app *App, suite *Suite) bool {
	ctx, cancel := context.WithTimeout(parent, 45*time.Second)
	init := app.WaitXRInit(ctx)
	cancel()
	if !suite.Check("[startup] the engine initialised OpenXR on our Monado build", init != nil && boolean(init["ok"]), init) {
		for _, line := range app.NonVRTestLines(12) {
			fmt.Fprintf(suite.Output, "      | %s\n", line)
		}
		return false
	}
	for _, hand := range []string{"left", "right"} {
		ctx, cancel := context.WithTimeout(parent, 20*time.Second)
		active := app.WaitControllerActive(ctx, hand)
		cancel()
		profile := ""
		if active != nil {
			profile = text(active["profile"])
		}
		suite.Check(fmt.Sprintf("[startup] %s controller became active, profile resolved", hand), active != nil && profile != "", profile)
	}
	return true
}

func phasePoseAndInputs(ctx context.Context, app *App, server *playspectra.Server, suite *Suite) {
	target := []float64{0, 1.6, -2.5}
	_ = server.MoveHead(ctx, map[string]any{"position": target}, 300)
	sleepContext(ctx, time.Second)
	pose := poseRequest(ctx, app, 8*time.Second)
	if !suite.Check("[pose] the app answers a pose request", pose != nil, pose) {
		return
	}
	want := StageToGlobal(target, eventMap(pose["origin"]))
	got := eventPosition(pose, "head")
	suite.Check("[pose] head lands at the commanded position", maxError(got, want) < positionTolerance, positionDetail(got, want))

	since := app.Count()
	_ = server.SetInput(ctx, "left", "/input/trigger/value", 0.7, 0)
	event := waitEvent(ctx, app, Axis("left", "trigger"), 8*time.Second, since)
	suite.Check("[input float] left trigger value reaches the engine's action system", event != nil && math.Abs(numberValue(event["value"])-0.7) < valueTolerance, event)

	since = app.Count()
	_ = server.SetInput(ctx, "right", "/input/squeeze/value", 0.8, 0)
	event = waitEvent(ctx, app, Axis("right", "grip"), 8*time.Second, since)
	suite.Check("[input float] right grip value reaches it too (both hands)", event != nil && math.Abs(numberValue(event["value"])-0.8) < valueTolerance, event)

	since = app.Count()
	_ = server.SetInput(ctx, "left", "/input/thumbstick/x", 0.5, 0)
	_ = server.SetInput(ctx, "left", "/input/thumbstick/y", -0.4, 0)
	event = waitEvent(ctx, app, func(event Event) bool {
		return text(event["t"]) == "vec2" && text(event["hand"]) == "left" && math.Abs(numberValue(event["y"])+0.4) < valueTolerance
	}, 8*time.Second, since)
	suite.Check("[input vec2] left thumbstick reaches it", event != nil, event)
	_ = server.SetInput(ctx, "left", "/input/thumbstick/x", 0.0, 0)
	_ = server.SetInput(ctx, "left", "/input/thumbstick/y", 0.0, 0)

	since = app.Count()
	_ = server.Press(ctx, "right", "a", 200)
	suite.Check("[input bool] right A press reaches it", waitEvent(ctx, app, Button("right", "pressed"), 8*time.Second, since) != nil, nil)
	suite.Check("[input bool] and its release", waitEvent(ctx, app, Button("right", "released"), 8*time.Second, since) != nil, nil)

	controllerTarget := []float64{-0.3, 1.2, -0.6}
	_ = server.MoveController(ctx, "left", map[string]any{"position": controllerTarget}, 300)
	sleepContext(ctx, 800*time.Millisecond)
	pose = poseRequest(ctx, app, 8*time.Second)
	if pose != nil {
		want = StageToGlobal(controllerTarget, eventMap(pose["origin"]))
		got = eventPosition(pose, "left")
		suite.Check("[input pose] left controller lands at the commanded position", maxError(got, want) < positionTolerance, positionDetail(got, want))
	}
}

func phaseCapture(ctx context.Context, server *playspectra.Server, suite *Suite) {
	_ = server.MoveHead(ctx, map[string]any{"position": []float64{0, 1.6, inspectorPosition[2] - 1.5}}, 400)
	_ = server.Look(ctx, 180, 400)
	sleepContext(ctx, 1200*time.Millisecond)
	first, firstStats := shoot(ctx, server)
	if !suite.Check("[capture] screenshot of a real engine app succeeds", boolean(first["ok"]), first["error"]) {
		return
	}
	suite.Check("[capture] the frame is a rendered scene, not a flat fill", pngstats.IsNonDegenerate(firstStats, 0, 0), fmt.Sprintf("%vx%v distinct=%v dominant=%v", firstStats["w"], firstStats["h"], firstStats["distinctColors"], firstStats["dominantFraction"]))
	_ = server.SetInput(ctx, "left", "/input/trigger/value", 0.85, 0)
	_ = server.SetInput(ctx, "right", "/input/squeeze/value", 0.65, 0)
	sleepContext(ctx, 1200*time.Millisecond)
	second, _ := shoot(ctx, server)
	suite.Check("[capture] an injected INPUT alone (camera unmoved) repaints the screen", boolean(second["ok"]) && text(second["hash"]) != text(first["hash"]), fmt.Sprintf("%v -> %v", first["hash"], second["hash"]))
	sleepContext(ctx, time.Second)
	third, _ := shoot(ctx, server)
	fourth, _ := shoot(ctx, server)
	suite.Check("[capture] negative control: two idle frames are identical", boolean(third["ok"]) && text(third["hash"]) == text(fourth["hash"]), fmt.Sprintf("%v vs %v", third["hash"], fourth["hash"]))
	_ = server.MoveHead(ctx, map[string]any{"position": []float64{0, 1.6, inspectorPosition[2] - 3}}, 400)
	sleepContext(ctx, time.Second)
	fifth, _ := shoot(ctx, server)
	suite.Check("[capture] moving the virtual HMD changes the rendered frame", boolean(fifth["ok"]) && text(fifth["hash"]) != text(fourth["hash"]), fmt.Sprintf("%v -> %v", fourth["hash"], fifth["hash"]))
}

func shoot(ctx context.Context, server *playspectra.Server) (map[string]any, map[string]any) {
	shot, _ := server.Screenshot(ctx, "dominant", 8000)
	stats := map[string]any{}
	if boolean(shot["ok"]) {
		stats, _ = pngstats.Stats(text(shot["path"]), pngstats.Options{})
	}
	return shot, stats
}

func phasePacing(ctx context.Context, capture *protocol.Client, suite *Suite) {
	first, err := capture.RequestLine(ctx, map[string]any{"cmd": "status"})
	if err != nil {
		suite.Skip("[pacing] observed frame rate", "status unavailable: "+err.Error())
		return
	}
	frames0, ok0 := nestedNumber(first, "capture", "framesObserved")
	started := time.Now()
	sleepContext(ctx, 3*time.Second)
	second, err := capture.RequestLine(ctx, map[string]any{"cmd": "status"})
	frames1, ok1 := nestedNumber(second, "capture", "framesObserved")
	if err != nil || !ok0 || !ok1 {
		suite.Skip("[pacing] observed frame rate", "framesObserved missing from layer status")
		return
	}
	fps := (frames1 - frames0) / math.Max(1e-6, time.Since(started).Seconds())
	suite.Check("[pacing] frames arrive at the device's declared rate, not the 20 FPS stub", fps > 40, fmt.Sprintf("measured %.1f fps over 3s (%.0f frames)", fps, frames1-frames0))
}

func phaseInteractions(ctx context.Context, app *App, server *playspectra.Server, suite *Suite) {
	pose := poseRequest(ctx, app, 8*time.Second)
	if pose == nil {
		suite.Skip("[interaction] all", "no pose snapshot to resolve STAGE coordinates against")
		return
	}
	origin := eventMap(pose["origin"])
	reach := func(hand string, global []float64, duration int) {
		_ = server.MoveController(ctx, hand, map[string]any{"position": GlobalToStage(global, origin)}, duration)
	}

	since := app.Count()
	reach("right", pushButton, 500)
	suite.Check("[interaction] right hand reaches the push button", waitEvent(ctx, app, HoverStart("PushButton", "right"), 10*time.Second, since) != nil, nil)
	since = app.Count()
	_ = server.SetInput(ctx, "right", "/input/trigger/value", 1.0, 0)
	suite.Check("[interaction] the trigger toggles it (the app's button logic ran)", waitEvent(ctx, app, Testbed("button_toggled", nil), 10*time.Second, since) != nil, nil)
	_ = server.SetInput(ctx, "right", "/input/trigger/value", 0.0, 0)

	since = app.Count()
	reach("left", grabCubeResting, 500)
	suite.Check("[interaction] left hand reaches the fallen cube", waitEvent(ctx, app, HoverStart("GrabCube", "left"), 10*time.Second, since) != nil, nil)
	since = app.Count()
	_ = server.SetInput(ctx, "left", "/input/squeeze/value", 0.9, 0)
	grabbed := waitEvent(ctx, app, Testbed("cube_grabbed", map[string]any{"hand": "left"}), 10*time.Second, since)
	suite.Check("[interaction] grip grabs it", grabbed != nil, nil)
	if grabbed != nil {
		since = app.Count()
		target := make([]float64, 3)
		for i, offset := range []float64{0.05, 0.225, -0.05} {
			target[i] = grabCubeResting[i] - vector(origin["pos"])[i] + offset
		}
		_ = server.MoveController(ctx, "left", map[string]any{"position": target}, 700)
		_ = server.SetInput(ctx, "left", "/input/squeeze/value", 0.0, 0)
		released := waitEvent(ctx, app, Testbed("cube_released", nil), 10*time.Second, since)
		moving := false
		if released != nil {
			for _, velocity := range vector(released["velocity"]) {
				moving = moving || math.Abs(velocity) > 0.01
			}
		}
		suite.Check("[interaction] releasing throws it with the carried velocity", released != nil && moving, released)
	}

	since = app.Count()
	reach("right", leverHandle, 500)
	suite.Check("[interaction] right hand reaches the lever handle", waitEvent(ctx, app, HoverStart("Lever", "right"), 10*time.Second, since) != nil, nil)
	since = app.Count()
	_ = server.SetInput(ctx, "right", "/input/squeeze/value", 0.9, 0)
	if suite.Check("[interaction] grip grabs the lever", waitEvent(ctx, app, Testbed("lever_grabbed", nil), 10*time.Second, since) != nil, nil) {
		since = app.Count()
		target := GlobalToStage(leverHandle, origin)
		target[0] += 0.25
		_ = server.MoveController(ctx, "right", map[string]any{"position": target}, 600)
		angle := waitEvent(ctx, app, func(event Event) bool {
			return text(event["t"]) == "testbed" && text(event["event"]) == "lever" && math.Abs(numberValue(event["angle"])) > 1
		}, 10*time.Second, since)
		suite.Check("[interaction] moving the hand drives the lever angle", angle != nil, angle)
		_ = server.SetInput(ctx, "right", "/input/squeeze/value", 0.0, 0)
	}
}

func PortOpen(host string, port int, timeout time.Duration) bool {
	connection, err := net.DialTimeout("tcp", net.JoinHostPort(host, strconv.Itoa(port)), timeout)
	if err != nil {
		return false
	}
	_ = connection.Close()
	return true
}

func WaitPort(ctx context.Context, host string, port int, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if PortOpen(host, port, time.Second) {
			return true
		}
		if !sleepContext(ctx, 400*time.Millisecond) {
			return false
		}
	}
	return false
}

func waitEvent(parent context.Context, app *App, predicate Predicate, timeout time.Duration, since int) Event {
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()
	return app.WaitFor(ctx, predicate, since)
}

func poseRequest(parent context.Context, app *App, timeout time.Duration) Event {
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()
	return app.Pose(ctx)
}

func sleepContext(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func eventMap(value any) Event {
	if event, ok := value.(map[string]any); ok {
		return Event(event)
	}
	if event, ok := value.(Event); ok {
		return event
	}
	return Event{}
}

func eventPosition(event Event, key string) []float64 { return vector(eventMap(event[key])["pos"]) }

func vector(value any) []float64 { return floats(value) }

func maxError(got, want []float64) float64 {
	if len(got) < 3 || len(want) < 3 {
		return math.Inf(1)
	}
	result := 0.0
	for i := 0; i < 3; i++ {
		result = math.Max(result, math.Abs(got[i]-want[i]))
	}
	return result
}

func positionDetail(got, want []float64) string {
	return fmt.Sprintf("err=%.4f got=%v want=%v", maxError(got, want), got, want)
}

func boolean(value any) bool { result, _ := value.(bool); return result }

func numberValue(value any) float64 {
	switch number := value.(type) {
	case float64:
		return number
	case int:
		return float64(number)
	default:
		return 0
	}
}

func nestedNumber(root map[string]any, keys ...string) (float64, bool) {
	var current any = root
	for _, key := range keys {
		object, ok := current.(map[string]any)
		if !ok {
			return 0, false
		}
		current, ok = object[key]
		if !ok {
			return 0, false
		}
	}
	value := numberValue(current)
	_, isFloat := current.(float64)
	_, isInt := current.(int)
	return value, isFloat || isInt
}

func envPort(name string, fallback int) int {
	if value, err := strconv.Atoi(os.Getenv(name)); err == nil && value > 0 {
		return value
	}
	return fallback
}
