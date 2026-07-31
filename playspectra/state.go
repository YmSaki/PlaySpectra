package playspectra

import (
	"encoding/json"
	"fmt"
	"math"
)

var (
	LeftInputPaths = []string{
		"/input/trigger/value", "/input/trigger/touch", "/input/squeeze/value",
		"/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
		"/input/thumbstick/touch", "/input/thumbrest/touch", "/button/x/click",
		"/button/x/touch", "/button/y/click", "/button/y/touch", "/input/menu/click",
	}
	RightInputPaths = []string{
		"/input/trigger/value", "/input/trigger/touch", "/input/squeeze/value",
		"/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
		"/input/thumbstick/touch", "/input/thumbrest/touch", "/button/a/click",
		"/button/a/touch", "/button/b/click", "/button/b/touch", "/input/system/click",
	}
)

type RelationFlags struct {
	PositionValid      bool `json:"position_valid"`
	OrientationValid   bool `json:"orientation_valid"`
	PositionTracked    bool `json:"position_tracked"`
	OrientationTracked bool `json:"orientation_tracked"`
}

var defaultRelationFlags = RelationFlags{true, true, true, true}

type Pose struct {
	Position        []float64     `json:"position"`
	Orientation     []float64     `json:"orientation"`
	LinearVelocity  []float64     `json:"linear_velocity,omitempty"`
	AngularVelocity []float64     `json:"angular_velocity,omitempty"`
	RelationFlags   RelationFlags `json:"relation_flags"`
}

type Clock struct {
	Mode         string `json:"mode"`
	TNS          *int64 `json:"t_ns,omitempty"`
	LogicalFrame *int64 `json:"logical_frame,omitempty"`
}

type HMD struct {
	Connected bool `json:"connected"`
	Head      Pose `json:"head"`
}

type Controller struct {
	Connected bool           `json:"connected"`
	Grip      Pose           `json:"grip"`
	Aim       Pose           `json:"aim"`
	Inputs    map[string]any `json:"inputs"`
}

type State struct {
	ProtocolVersion int        `json:"protocol_version,omitempty"`
	Sequence        uint64     `json:"sequence"`
	Clock           Clock      `json:"clock"`
	HMD             HMD        `json:"hmd"`
	Left            Controller `json:"left"`
	Right           Controller `json:"right"`
}

func pose(position, orientation []float64) Pose {
	return Pose{Position: cloneFloats(position), Orientation: cloneFloats(orientation), RelationFlags: defaultRelationFlags}
}

func defaultInputs(paths []string) map[string]any {
	out := make(map[string]any, len(paths))
	for _, path := range paths {
		if hasNumericSuffix(path) {
			out[path] = float64(0)
		} else {
			out[path] = false
		}
	}
	return out
}

func hasNumericSuffix(path string) bool {
	return len(path) > 0 && (endsWith(path, "/value") || endsWith(path, "/x") || endsWith(path, "/y"))
}

func endsWith(s, suffix string) bool {
	if len(s) < len(suffix) {
		return false
	}
	return s[len(s)-len(suffix):] == suffix
}

type Model struct {
	HeadPos  []float64
	HeadQuat []float64
	Left     Controller
	Right    Controller
}

func DefaultModel() *Model {
	leftPos := []float64{-0.2, 1.3, -0.5}
	rightPos := []float64{0.2, 1.3, -0.5}
	identity := []float64{0, 0, 0, 1}
	return &Model{
		HeadPos: cloneFloats([]float64{0, 1.6, 0}), HeadQuat: cloneFloats(identity),
		Left:  Controller{Connected: true, Grip: pose(leftPos, identity), Aim: pose(leftPos, identity), Inputs: defaultInputs(LeftInputPaths)},
		Right: Controller{Connected: true, Grip: pose(rightPos, identity), Aim: pose(rightPos, identity), Inputs: defaultInputs(RightInputPaths)},
	}
}

func (m *Model) Hand(name string) (*Controller, error) {
	switch name {
	case "left":
		return &m.Left, nil
	case "right":
		return &m.Right, nil
	default:
		return nil, fmt.Errorf("hand must be 'left' or 'right', got %q", name)
	}
}

func (m *Model) Snapshot(sequence uint64) State {
	return State{
		ProtocolVersion: 1, Sequence: sequence, Clock: Clock{Mode: "realtime"},
		HMD:  HMD{Connected: true, Head: pose(m.HeadPos, m.HeadQuat)},
		Left: cloneController(m.Left), Right: cloneController(m.Right),
	}
}

func clonePose(p Pose) Pose {
	out := p
	out.Position, out.Orientation = cloneFloats(p.Position), cloneFloats(p.Orientation)
	out.LinearVelocity, out.AngularVelocity = cloneFloats(p.LinearVelocity), cloneFloats(p.AngularVelocity)
	return out
}

func cloneController(c Controller) Controller {
	out := c
	out.Grip, out.Aim = clonePose(c.Grip), clonePose(c.Aim)
	out.Inputs = make(map[string]any, len(c.Inputs))
	for k, v := range c.Inputs {
		out.Inputs[k] = v
	}
	return out
}

func (m *Model) Seed(raw map[string]any) {
	state := raw
	if wrapped, ok := raw["state"].(map[string]any); ok {
		state = wrapped
	}
	if hmd, ok := state["hmd"].(map[string]any); ok {
		if head, ok := hmd["head"].(map[string]any); ok {
			if p, ok := floatSlice(head["position"], 3); ok {
				m.HeadPos = p
			}
			if q, ok := floatSlice(head["orientation"], 4); ok {
				m.HeadQuat = q
			}
		}
	}
	for name, c := range map[string]*Controller{"left": &m.Left, "right": &m.Right} {
		if value, ok := state[name].(map[string]any); ok {
			if connected, ok := value["connected"].(bool); ok {
				c.Connected = connected
			}
			seedPose := func(key string, dst *Pose) {
				if v, ok := value[key].(map[string]any); ok {
					if p, ok := floatSlice(v["position"], 3); ok {
						dst.Position = p
					}
					if q, ok := floatSlice(v["orientation"], 4); ok {
						dst.Orientation = q
					}
				}
			}
			seedPose("grip", &c.Grip)
			seedPose("aim", &c.Aim)
			if inputs, ok := value["inputs"].(map[string]any); ok {
				for path := range c.Inputs {
					if v, exists := inputs[path]; exists {
						c.Inputs[path] = v
					}
				}
			}
		}
	}
}

func floatSlice(value any, want int) ([]float64, bool) {
	var values []any
	switch v := value.(type) {
	case []any:
		values = v
	case []float64:
		out := cloneFloats(v)
		return out, len(out) == want
	default:
		return nil, false
	}
	if len(values) != want {
		return nil, false
	}
	out := make([]float64, want)
	for i, value := range values {
		f, ok := asFloat(value)
		if !ok || math.IsNaN(f) || math.IsInf(f, 0) {
			return nil, false
		}
		out[i] = f
	}
	return out, true
}

func asFloat(value any) (float64, bool) {
	switch v := value.(type) {
	case float64:
		return v, true
	case float32:
		return float64(v), true
	case int:
		return float64(v), true
	case int64:
		return float64(v), true
	case json.Number:
		f, err := v.Float64()
		return f, err == nil
	default:
		return 0, false
	}
}

func (s State) Validate() error {
	if s.Sequence == 0 { /* sequence 0 is valid for an initial adapter snapshot */
	}
	if err := validatePose("hmd.head", s.HMD.Head, s.HMD.Connected); err != nil {
		return err
	}
	if err := validateController("left", s.Left, LeftInputPaths); err != nil {
		return err
	}
	if err := validateController("right", s.Right, RightInputPaths); err != nil {
		return err
	}
	return nil
}

func validatePose(name string, p Pose, connected bool) error {
	if !connected {
		return nil
	}
	if len(p.Position) != 3 || len(p.Orientation) != 4 {
		return fmt.Errorf("validation_error: %s pose dimensions", name)
	}
	return nil
}

func validateController(name string, c Controller, paths []string) error {
	if !c.Connected {
		return nil
	}
	if err := validatePose(name+".grip", c.Grip, true); err != nil {
		return err
	}
	if err := validatePose(name+".aim", c.Aim, true); err != nil {
		return err
	}
	for _, path := range paths {
		value, ok := c.Inputs[path]
		if !ok {
			return fmt.Errorf("validation_error: missing:%s hand:%s", path, name)
		}
		if hasNumericSuffix(path) {
			number, ok := asFloat(value)
			if !ok {
				return fmt.Errorf("validation_error: input:%s hand:%s requires number", path, name)
			}
			if endsWith(path, "/value") && (number < 0 || number > 1) {
				return fmt.Errorf("validation_error: input:%s out of range", path)
			}
			if (endsWith(path, "/x") || endsWith(path, "/y")) && (number < -1 || number > 1) {
				return fmt.Errorf("validation_error: input:%s out of range", path)
			}
		} else if _, ok := value.(bool); !ok {
			return fmt.Errorf("validation_error: input:%s hand:%s requires boolean", path, name)
		}
	}
	return nil
}

func (s State) Raw() map[string]any {
	b, _ := json.Marshal(s)
	var out map[string]any
	_ = json.Unmarshal(b, &out)
	return out
}
