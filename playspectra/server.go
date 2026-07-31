package playspectra

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"os"
	"reflect"
	"time"
)

// Transport is the small boundary shared by the adapter client and test
// doubles. The concrete implementation is protocol.Client.
type Transport interface {
	Request(context.Context, map[string]any) (map[string]any, error)
	SendOnly(context.Context, map[string]any) error
}

type CaptureTransport interface {
	RequestLine(context.Context, map[string]any) (map[string]any, error)
}

type ServerOption func(*Server)

func WithRate(rateHz float64) ServerOption {
	return func(s *Server) {
		if rateHz > 0 {
			s.dt = time.Duration(float64(time.Second) / rateHz)
		}
	}
}

func WithLogger(logger func(string, ...any)) ServerOption {
	return func(s *Server) {
		if logger != nil {
			s.logf = logger
		}
	}
}

func WithCapture(c CaptureTransport) ServerOption { return func(s *Server) { s.Capture = c } }

// WithSleeper makes interpolation deterministic in unit tests and is also
// useful to callers that want to generate frames without real-time delays.
func WithSleeper(sleeper func(time.Duration)) ServerOption {
	return func(s *Server) {
		if sleeper != nil {
			s.sleep = sleeper
		}
	}
}

type Assertion struct {
	Name   string
	OK     bool
	Actual any
}

type Server struct {
	Client     Transport
	Capture    CaptureTransport
	Model      *Model
	Seq        uint64
	dt         time.Duration
	logf       func(string, ...any)
	sleep      func(time.Duration)
	assertions []Assertion
	captures   map[string]CaptureReference
	requestSeq uint64
}

type CaptureReference struct {
	Hash string `json:"hash"`
	Path string `json:"path"`
}

func NewServer(client Transport, options ...ServerOption) *Server {
	s := &Server{
		Client: client, Model: DefaultModel(), dt: time.Second / 60,
		logf:  func(format string, args ...any) { log.Printf(format, args...) },
		sleep: time.Sleep, captures: map[string]CaptureReference{},
	}
	for _, option := range options {
		option(s)
	}
	return s
}

func (s *Server) nextID(prefix string) string {
	s.requestSeq++
	return fmt.Sprintf("go-%s-%d", prefix, s.requestSeq)
}

func (s *Server) request(ctx context.Context, req map[string]any) (map[string]any, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if _, ok := req["request_id"]; !ok {
		req["request_id"] = s.nextID("request")
	}
	return s.Client.Request(ctx, req)
}

func (s *Server) Hello(ctx context.Context, role string) error {
	if role == "" {
		role = "writer"
	}
	r, err := s.request(ctx, map[string]any{
		"cmd": "hello", "request_id": s.nextID("hello"),
		"protocol_version": 1, "role": role,
	})
	if err != nil {
		return fmt.Errorf("hello: %w", err)
	}
	if ok, _ := r["ok"].(bool); !ok {
		return protocolError(r, "hello failed")
	}
	g, err := s.GetState(ctx)
	if err != nil {
		return fmt.Errorf("seed state: %w", err)
	}
	s.Model.Seed(g)
	if seq, ok := numberToUint(g["sequence"]); ok {
		s.Seq = seq
	}
	return nil
}

func protocolError(response map[string]any, fallback string) error {
	if response == nil {
		return fmt.Errorf("%s: empty response", fallback)
	}
	if message, ok := response["error"].(string); ok {
		return fmt.Errorf("%s: %s", fallback, message)
	}
	return fmt.Errorf("%s: %v", fallback, response)
}

func (s *Server) GetState(ctx context.Context) (map[string]any, error) {
	r, err := s.request(ctx, map[string]any{"cmd": "get_state", "request_id": s.nextID("get-state")})
	if err != nil {
		return nil, fmt.Errorf("get_state: %w", err)
	}
	if ok, present := r["ok"].(bool); present && !ok {
		return nil, protocolError(r, "get_state failed")
	}
	state, ok := r["state"].(map[string]any)
	if !ok {
		return nil, fmt.Errorf("get_state: response has no state")
	}
	return state, nil
}

func (s *Server) emit(ctx context.Context) error {
	s.Seq++
	state := s.Model.Snapshot(s.Seq)
	return s.Client.SendOnly(ctx, map[string]any{
		"cmd": "set_state", "request_id": fmt.Sprintf("%s-%d", s.nextID("set-state"), s.Seq),
		"state": state,
	})
}

// SetState sends one complete VirtualDeviceState snapshot through the same
// writer channel used by high-level operations. It is intentionally exposed
// separately from the interpolation helpers so callers can replay or inject
// an exact frame without linking against the C++ runtime.
func (s *Server) SetState(ctx context.Context, state map[string]any) error {
	if state == nil {
		return fmt.Errorf("set_state requires a state object")
	}
	if _, present := state["protocol_version"]; !present {
		state["protocol_version"] = 1
	}
	if _, present := state["clock"]; !present {
		state["clock"] = map[string]any{"mode": "realtime"}
	}
	if err := validateRawState(state); err != nil {
		return err
	}
	if sequence, ok := numberToUint(state["sequence"]); ok && sequence > s.Seq {
		s.Seq = sequence
	}
	if err := s.Client.SendOnly(ctx, map[string]any{
		"cmd": "set_state", "request_id": s.nextID("set-state-direct"), "state": state,
	}); err != nil {
		return err
	}
	s.Model.Seed(map[string]any{"state": state})
	return nil
}

func validateRawState(raw map[string]any) error {
	for _, key := range []string{"sequence", "clock", "hmd", "left", "right"} {
		if _, present := raw[key]; !present {
			return fmt.Errorf("validation_error: missing:%s", key)
		}
	}
	data, err := json.Marshal(raw)
	if err != nil {
		return fmt.Errorf("set_state encode: %w", err)
	}
	var state State
	if err := json.Unmarshal(data, &state); err != nil {
		return fmt.Errorf("set_state decode: %w", err)
	}
	if err := state.Validate(); err != nil {
		return err
	}
	return nil
}

func (s *Server) stream(ctx context.Context, durationMS int, apply func(float64)) error {
	seconds := float64(durationMS) / 1000
	// Python's round(), used by the reference implementation, resolves exact
	// half-way values to the nearest even integer.
	n := int(math.RoundToEven(seconds / s.dt.Seconds()))
	if n < 1 {
		n = 1
	}
	for i := 1; i <= n; i++ {
		apply(float64(i) / float64(n))
		if err := s.emit(ctx); err != nil {
			return err
		}
		if s.sleep != nil {
			s.sleep(s.dt)
		}
	}
	return nil
}

func (s *Server) MoveHead(ctx context.Context, to map[string]any, durationMS int) error {
	p0, q0 := cloneFloats(s.Model.HeadPos), cloneFloats(s.Model.HeadQuat)
	p1, q1 := p0, q0
	if position, ok := floatSlice(to["position"], 3); ok {
		p1 = position
	}
	if orientation, ok := floatSlice(to["orientation"], 4); ok {
		q1 = QuatNorm(orientation)
	}
	return s.stream(ctx, durationMS, func(t float64) {
		s.Model.HeadPos = Lerp3(p0, p1, t)
		s.Model.HeadQuat = Slerp(q0, q1, t)
	})
}

func (s *Server) Look(ctx context.Context, yawDeg float64, durationMS int) error {
	q0 := cloneFloats(s.Model.HeadQuat)
	q1 := QuatNorm(QuatMul(QuatYaw(yawDeg*math.Pi/180), q0))
	return s.stream(ctx, durationMS, func(t float64) { s.Model.HeadQuat = Slerp(q0, q1, t) })
}

func (s *Server) WalkForward(ctx context.Context, speed float64, durationMS int, hand string) error {
	return s.setAxis(ctx, hand, "/input/thumbstick/y", clamp(speed, -1, 1), durationMS)
}

func (s *Server) Strafe(ctx context.Context, speed float64, durationMS int, hand string) error {
	return s.setAxis(ctx, hand, "/input/thumbstick/x", clamp(speed, -1, 1), durationMS)
}

func (s *Server) setAxis(ctx context.Context, hand, path string, value float64, durationMS int) error {
	controller, err := s.Model.Hand(hand)
	if err != nil {
		return err
	}
	if _, ok := controller.Inputs[path]; !ok {
		return fmt.Errorf("unknown input %q for %s", path, hand)
	}
	if err := s.stream(ctx, durationMS, func(float64) { controller.Inputs[path] = value }); err != nil {
		return err
	}
	controller.Inputs[path] = float64(0)
	return s.emit(ctx)
}

func (s *Server) SetTrigger(ctx context.Context, hand string, value float64, durationMS int) error {
	controller, err := s.Model.Hand(hand)
	if err != nil {
		return err
	}
	value = clamp(value, 0, 1)
	return s.stream(ctx, durationMS, func(float64) { controller.Inputs["/input/trigger/value"] = value })
}

func (s *Server) MoveController(ctx context.Context, hand string, to map[string]any, durationMS int) error {
	controller, err := s.Model.Hand(hand)
	if err != nil {
		return err
	}
	gp0, gq0 := cloneFloats(controller.Grip.Position), cloneFloats(controller.Grip.Orientation)
	ap0, aq0 := cloneFloats(controller.Aim.Position), cloneFloats(controller.Aim.Orientation)
	p1, q1 := gp0, gq0
	if position, ok := floatSlice(to["position"], 3); ok {
		p1 = position
	}
	if orientation, ok := floatSlice(to["orientation"], 4); ok {
		q1 = QuatNorm(orientation)
	}
	return s.stream(ctx, durationMS, func(t float64) {
		controller.Grip.Position, controller.Grip.Orientation = Lerp3(gp0, p1, t), Slerp(gq0, q1, t)
		controller.Aim.Position, controller.Aim.Orientation = Lerp3(ap0, p1, t), Slerp(aq0, q1, t)
	})
}

func (s *Server) SetInput(ctx context.Context, hand, path string, value any, durationMS int) error {
	controller, err := s.Model.Hand(hand)
	if err != nil {
		return err
	}
	if _, ok := controller.Inputs[path]; !ok {
		return fmt.Errorf("unknown input %q for %s (declared: %v)", path, hand, keys(controller.Inputs))
	}
	value, err = normalizeInputValue(path, value)
	if err != nil {
		return err
	}
	if durationMS > 0 {
		return s.stream(ctx, durationMS, func(float64) { controller.Inputs[path] = value })
	}
	controller.Inputs[path] = value
	return s.emit(ctx)
}

func normalizeInputValue(path string, value any) (any, error) {
	if hasNumericSuffix(path) {
		number, ok := asFloat(value)
		if !ok {
			return nil, fmt.Errorf("input %q requires a number", path)
		}
		if endsWith(path, "/value") {
			number = clamp(number, 0, 1)
		} else {
			number = clamp(number, -1, 1)
		}
		return number, nil
	}
	if boolean, ok := value.(bool); ok {
		return boolean, nil
	}
	if number, ok := asFloat(value); ok {
		return number != 0, nil
	}
	return nil, fmt.Errorf("input %q requires a boolean", path)
}

func (s *Server) Press(ctx context.Context, hand, button string, ms int) error {
	controller, err := s.Model.Hand(hand)
	if err != nil {
		return err
	}
	click, touch := "/button/"+button+"/click", "/button/"+button+"/touch"
	if _, ok := controller.Inputs[click]; !ok {
		return fmt.Errorf("unknown button %q for %s", button, hand)
	}
	controller.Inputs[click], controller.Inputs[touch] = true, true
	if err := s.emit(ctx); err != nil {
		return err
	}
	if ms > 0 && s.sleep != nil {
		s.sleep(time.Duration(ms) * time.Millisecond)
	}
	controller.Inputs[click], controller.Inputs[touch] = false, false
	return s.emit(ctx)
}

func (s *Server) Wait(ms int) {
	if ms > 0 && s.sleep != nil {
		s.sleep(time.Duration(ms) * time.Millisecond)
	}
}

func (s *Server) Reset(ctx context.Context) error {
	r, err := s.request(ctx, map[string]any{"cmd": "reset", "request_id": s.nextID("reset")})
	if err != nil {
		return fmt.Errorf("reset: %w", err)
	}
	if ok, present := r["ok"].(bool); present && !ok {
		return protocolError(r, "reset failed")
	}
	g, err := s.GetState(ctx)
	if err != nil {
		return err
	}
	s.Model.Seed(g)
	return nil
}

func clamp(value, min, max float64) float64 { return math.Max(min, math.Min(max, value)) }

func keys(values map[string]any) []string {
	out := make([]string, 0, len(values))
	for key := range values {
		out = append(out, key)
	}
	return out
}

// Screenshot returns the capture result and a short SHA-256 of the PNG, as
// used by the visual assertion API.
func (s *Server) Screenshot(ctx context.Context, eye string, timeoutMS int) (map[string]any, error) {
	if s.Capture == nil {
		return map[string]any{"ok": false, "error": "no capture channel (need --capture-port + a layer-loaded app)"}, nil
	}
	if eye == "" {
		eye = "left"
	}
	r, err := s.Capture.RequestLine(ctx, map[string]any{"cmd": "screenshot", "eye": eye, "timeoutMs": timeoutMS})
	if err != nil {
		return map[string]any{"ok": false, "error": "capture request failed: " + err.Error()}, nil
	}
	if ok, _ := r["ok"].(bool); !ok {
		return map[string]any{"ok": false, "error": fmt.Sprintf("screenshot not ok: %v", r)}, nil
	}
	path, _ := r["path"].(string)
	if _, err := os.Stat(path); err != nil {
		return map[string]any{"ok": false, "error": "screenshot path missing: " + path}, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return map[string]any{"ok": false, "error": "read PNG failed: " + err.Error()}, nil
	}
	sum := sha256.Sum256(data)
	return map[string]any{"ok": true, "hash": hex.EncodeToString(sum[:])[:16], "path": path}, nil
}

func (s *Server) CaptureRef(ctx context.Context, name, eye string) bool {
	r, err := s.Screenshot(ctx, eye, 8000)
	ok, _ := r["ok"].(bool)
	if err == nil && ok {
		s.captures[name] = CaptureReference{Hash: stringValue(r["hash"]), Path: stringValue(r["path"])}
	}
	s.recordAssertion("capture["+name+"]", ok, firstString(r["hash"], r["error"]))
	return ok
}

func (s *Server) AssertCapture(ctx context.Context, ref, op, eye, name string, timeoutMS, pollMS int) bool {
	base, exists := s.captures[ref]
	label := name
	if label == "" {
		label = fmt.Sprintf("capture %s vs ref[%s]", op, ref)
	}
	if !exists {
		s.recordAssertion(label, false, "no reference "+ref)
		return false
	}
	ok, detail := s.poll(timeoutMS, pollMS, func() (bool, any) {
		r, _ := s.Screenshot(ctx, eye, 8000)
		if !boolValue(r["ok"]) {
			return false, r["error"]
		}
		same := stringValue(r["hash"]) == base.Hash
		return (op == "stable" && same) || (op == "changed" && !same), fmt.Sprintf("now=%s ref=%s", r["hash"], base.Hash)
	})
	s.recordAssertion(label, ok, detail)
	return ok
}

func (s *Server) poll(timeoutMS, pollMS int, probe func() (bool, any)) (bool, any) {
	start := time.Now()
	deadline := start.Add(time.Duration(maxInt(timeoutMS, 0)) * time.Millisecond)
	for {
		ok, actual := probe()
		if ok {
			return true, actual
		}
		if timeoutMS <= 0 || !time.Now().Before(deadline) {
			return false, actual
		}
		if s.sleep != nil {
			interval := pollMS
			if interval < 0 {
				interval = 0
			}
			s.sleep(time.Duration(interval) * time.Millisecond)
		}
	}
}

func (s *Server) AssertState(ctx context.Context, path []any, op string, value any, tol float64, name string, timeoutMS, pollMS int) (bool, error) {
	ok, actual, err := s.pollState(ctx, path, op, value, tol, timeoutMS, pollMS)
	label := name
	if label == "" {
		label = fmt.Sprintf("%v %s %v", path, op, value)
	}
	s.recordAssertion(label, ok, actual)
	return ok, err
}

func (s *Server) WaitFor(ctx context.Context, path []any, op string, value any, tol float64, timeoutMS, pollMS int, name string) (bool, error) {
	ok, actual, err := s.pollState(ctx, path, op, value, tol, timeoutMS, pollMS)
	label := name
	if label == "" {
		label = fmt.Sprintf("wait %v %s %v", path, op, value)
	}
	s.recordAssertion(label, ok, actual)
	return ok, err
}

func (s *Server) pollState(ctx context.Context, path []any, op string, value any, tol float64, timeoutMS, pollMS int) (bool, any, error) {
	var last any
	ok, detail := s.poll(timeoutMS, pollMS, func() (bool, any) {
		state, err := s.GetState(ctx)
		if err != nil {
			last = err.Error()
			return false, last
		}
		last = Resolve(state, path)
		return Compare(last, op, value, tol), last
	})
	return ok, detail, nil
}

func (s *Server) recordAssertion(name string, ok bool, actual any) {
	s.assertions = append(s.assertions, Assertion{Name: name, OK: ok, Actual: actual})
}

func (s *Server) ResetAssertions() { s.assertions = nil }

func (s *Server) Assertions() []Assertion { return append([]Assertion(nil), s.assertions...) }

func (s *Server) Summary() map[string]any {
	passed := 0
	failures := []string{}
	for _, assertion := range s.assertions {
		if assertion.OK {
			passed++
		} else {
			failures = append(failures, assertion.Name)
		}
	}
	return map[string]any{"asserts": len(s.assertions), "passed": passed, "failed": len(s.assertions) - passed, "ok": len(s.assertions) == 0 || passed == len(s.assertions), "failures": failures}
}

func Resolve(node any, path []any) any {
	current := node
	for _, key := range path {
		switch value := current.(type) {
		case map[string]any:
			name, ok := key.(string)
			if !ok {
				return nil
			}
			current = value[name]
		case []any:
			index, ok := numberToInt(key)
			if !ok || index < 0 || index >= len(value) {
				return nil
			}
			current = value[index]
		default:
			return nil
		}
	}
	return current
}

func Compare(actual any, op string, expected any, tolerance float64) bool {
	switch op {
	case "near":
		a, aok := asFloat(actual)
		b, bok := asFloat(expected)
		return aok && bok && math.Abs(a-b) <= tolerance
	case "eq":
		if a, aok := asFloat(actual); aok {
			if b, bok := asFloat(expected); bok {
				return a == b
			}
		}
		return reflect.DeepEqual(actual, expected)
	case "ne":
		return !Compare(actual, "eq", expected, tolerance)
	case "gt", "lt":
		a, aok := asFloat(actual)
		b, bok := asFloat(expected)
		if !aok || !bok {
			return false
		}
		if op == "gt" {
			return a > b
		}
		return a < b
	case "true":
		value, ok := actual.(bool)
		return ok && value
	case "false":
		value, ok := actual.(bool)
		return ok && !value
	default:
		return false
	}
}

func (s *Server) RunStep(ctx context.Context, step map[string]any) (any, error) {
	cmd := stringValue(step["cmd"])
	str := func(key, fallback string) string {
		if value, ok := step[key].(string); ok {
			return value
		}
		return fallback
	}
	integer := func(key string, fallback int) int {
		if value, ok := step[key]; ok {
			if v, ok := numberToInt(value); ok {
				return v
			}
		}
		return fallback
	}
	number := func(key string, fallback float64) float64 {
		if value, ok := step[key]; ok {
			if v, ok := asFloat(value); ok {
				return v
			}
		}
		return fallback
	}
	to := func() map[string]any {
		if value, ok := step["to"].(map[string]any); ok {
			return value
		}
		return map[string]any{}
	}
	s.logf("step: %s", cmd)
	switch cmd {
	case "hello":
		return nil, s.Hello(ctx, str("role", "writer"))
	case "move_head":
		return nil, s.MoveHead(ctx, to(), integer("duration_ms", 500))
	case "look":
		return nil, s.Look(ctx, number("yaw_deg", 0), integer("duration_ms", 500))
	case "walk_forward":
		return nil, s.WalkForward(ctx, number("speed", 1), integer("duration_ms", 1000), str("hand", "left"))
	case "strafe":
		return nil, s.Strafe(ctx, number("speed", 1), integer("duration_ms", 1000), str("hand", "left"))
	case "trigger", "set_trigger":
		return nil, s.SetTrigger(ctx, str("hand", "right"), number("value", 1), integer("duration_ms", 200))
	case "move_controller":
		return nil, s.MoveController(ctx, str("hand", "right"), to(), integer("duration_ms", 400))
	case "set_input":
		inputValue := step["value"]
		if inputValue == nil {
			inputValue = float64(0)
		}
		return nil, s.SetInput(ctx, str("hand", "right"), str("path", ""), inputValue, integer("duration_ms", 0))
	case "press":
		return nil, s.Press(ctx, str("hand", "right"), str("button", "a"), integer("ms", 120))
	case "wait":
		s.Wait(integer("ms", 100))
		return nil, nil
	case "reset":
		return nil, s.Reset(ctx)
	case "set_state":
		state, ok := step["state"].(map[string]any)
		if !ok {
			return nil, fmt.Errorf("set_state requires state object")
		}
		return nil, s.SetState(ctx, state)
	case "get_state", "status":
		return s.GetState(ctx)
	case "assert":
		path := pathValue(step["get"])
		ok, err := s.AssertState(ctx, path, str("op", "near"), step["value"], number("tol", 1e-3), str("name", ""), integer("timeout_ms", 0), integer("poll_ms", 50))
		return ok, err
	case "wait_for":
		path := pathValue(step["get"])
		ok, err := s.WaitFor(ctx, path, str("op", "true"), step["value"], number("tol", 1e-3), integer("timeout_ms", 5000), integer("poll_ms", 50), str("name", ""))
		return ok, err
	case "capture":
		return s.CaptureRef(ctx, str("name", "ref"), str("eye", "left")), nil
	case "assert_capture":
		return s.AssertCapture(ctx, str("ref", "ref"), str("op", "changed"), str("eye", "left"), str("name", ""), integer("timeout_ms", 0), integer("poll_ms", 100)), nil
	default:
		return nil, fmt.Errorf("unknown scenario cmd: %q", cmd)
	}
}

func pathValue(value any) []any {
	if values, ok := value.([]any); ok {
		return values
	}
	if value == nil {
		return nil
	}
	return []any{value}
}

func (s *Server) RunScenario(ctx context.Context, scenario map[string]any) (map[string]any, error) {
	steps := []any{}
	if raw, present := scenario["steps"]; present {
		var ok bool
		steps, ok = raw.([]any)
		if !ok {
			return s.Summary(), fmt.Errorf("scenario steps must be an array")
		}
	}
	if len(steps) == 0 || stringValue(stepMap(steps[0])["cmd"]) != "hello" {
		if err := s.Hello(ctx, "writer"); err != nil {
			return s.Summary(), err
		}
	}
	for _, raw := range steps {
		step, ok := raw.(map[string]any)
		if !ok {
			return s.Summary(), fmt.Errorf("scenario step must be an object")
		}
		if _, err := s.RunStep(ctx, step); err != nil {
			return s.Summary(), err
		}
	}
	return s.Summary(), nil
}

func stepMap(value any) map[string]any {
	if step, ok := value.(map[string]any); ok {
		return step
	}
	return map[string]any{}
}

func (s *Server) RunScenarioJSON(ctx context.Context, data []byte) (map[string]any, error) {
	var scenario map[string]any
	if err := json.Unmarshal(data, &scenario); err != nil {
		return nil, fmt.Errorf("decode scenario: %w", err)
	}
	if scenario == nil {
		return nil, fmt.Errorf("decode scenario: top level must be an object")
	}
	return s.RunScenario(ctx, scenario)
}

func (s *Server) log(format string, args ...any) {
	if s.logf != nil {
		s.logf(format, args...)
	}
}

func numberToUint(value any) (uint64, bool) {
	f, ok := asFloat(value)
	return uint64(f), ok && f >= 0 && f == math.Trunc(f)
}
func numberToInt(value any) (int, bool) {
	f, ok := asFloat(value)
	return int(f), ok && f == math.Trunc(f)
}
func stringValue(value any) string { v, _ := value.(string); return v }
func boolValue(value any) bool     { v, _ := value.(bool); return v }
func firstString(values ...any) string {
	for _, value := range values {
		if text := stringValue(value); text != "" {
			return text
		}
	}
	return ""
}
func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// MarshalState is a convenience for CLI/MCP adapters and keeps JSON output
// compact and UTF-8 safe.
func MarshalState(state any) string { data, _ := json.Marshal(state); return string(data) }
