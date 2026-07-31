package playspectra

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"time"
)

type RecordingFrame struct {
	TMS   float64        `json:"t_ms"`
	State map[string]any `json:"state"`
}

type Recording struct {
	Name   string           `json:"name"`
	RateHz float64          `json:"rate_hz"`
	Frames []RecordingFrame `json:"frames"`
}

type Recorder struct {
	Client Transport
	RateHz float64
	Frames []RecordingFrame
	count  uint64
}

func NewRecorder(client Transport, rateHz float64) *Recorder {
	if rateHz <= 0 {
		rateHz = 60
	}
	return &Recorder{Client: client, RateHz: rateHz}
}

func (r *Recorder) Hello(ctx context.Context) error {
	response, err := r.Client.Request(ctx, map[string]any{
		"cmd": "hello", "request_id": "go-rec-hello", "protocol_version": 1, "role": "observer",
	})
	if err != nil {
		return fmt.Errorf("observer hello: %w", err)
	}
	if !boolValue(response["ok"]) || stringValue(response["role_granted"]) != "observer" {
		return protocolError(response, "observer hello failed")
	}
	return nil
}

func (r *Recorder) Sample(ctx context.Context, started time.Time) error {
	r.count++
	response, err := r.Client.Request(ctx, map[string]any{"cmd": "get_state", "request_id": fmt.Sprintf("go-rec-%d", r.count)})
	if err != nil {
		return err
	}
	state, ok := response["state"].(map[string]any)
	if !ok {
		return fmt.Errorf("record get_state: response has no state")
	}
	r.Frames = append(r.Frames, RecordingFrame{TMS: float64(time.Since(started).Microseconds()) / 1000, State: state})
	return nil
}

func (r *Recorder) RecordFor(ctx context.Context, duration time.Duration) error {
	started := time.Now()
	deadline := started.Add(duration)
	interval := time.Duration(float64(time.Second) / r.RateHz)
	if interval <= 0 {
		interval = time.Millisecond
	}
	for time.Now().Before(deadline) {
		if err := r.Sample(ctx, started); err != nil {
			return err
		}
		timer := time.NewTimer(interval)
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
	return nil
}

func (r *Recorder) Recording(name string) Recording {
	return Recording{Name: name, RateHz: r.RateHz, Frames: append([]RecordingFrame(nil), r.Frames...)}
}

func SaveRecording(path string, recording Recording) error {
	data, err := json.MarshalIndent(recording, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(path, append(data, '\n'), 0o644); err != nil {
		return fmt.Errorf("save recording: %w", err)
	}
	return nil
}

func LoadRecording(path string) (Recording, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Recording{}, fmt.Errorf("read recording: %w", err)
	}
	var recording Recording
	if err := json.Unmarshal(data, &recording); err != nil {
		return Recording{}, fmt.Errorf("decode recording: %w", err)
	}
	if recording.RateHz <= 0 {
		recording.RateHz = 60
	}
	return recording, nil
}

type Replayer struct {
	Client Transport
	Seq    uint64
}

func NewReplayer(client Transport) *Replayer { return &Replayer{Client: client} }

func (r *Replayer) Hello(ctx context.Context) error {
	response, err := r.Client.Request(ctx, map[string]any{
		"cmd": "hello", "request_id": "go-rep-hello", "protocol_version": 1, "role": "writer",
	})
	if err != nil {
		return fmt.Errorf("writer hello: %w", err)
	}
	if !boolValue(response["ok"]) || stringValue(response["role_granted"]) != "writer" {
		return protocolError(response, "writer hello failed")
	}
	return nil
}

func (r *Replayer) Play(ctx context.Context, recording Recording, logf func(string, ...any)) (int, error) {
	if len(recording.Frames) == 0 {
		return 0, nil
	}
	response, err := r.Client.Request(ctx, map[string]any{"cmd": "get_state", "request_id": "go-rep-seq"})
	if err != nil {
		return 0, err
	}
	if state, ok := response["state"].(map[string]any); ok {
		if seq, valid := numberToUint(state["sequence"]); valid && seq > r.Seq {
			r.Seq = seq
		}
	}
	start := time.Now()
	base := recording.Frames[0].TMS
	for index, frame := range recording.Frames {
		target := start.Add(time.Duration((frame.TMS - base) * float64(time.Millisecond)))
		if wait := time.Until(target); wait > 0 {
			timer := time.NewTimer(wait)
			select {
			case <-ctx.Done():
				timer.Stop()
				return index, ctx.Err()
			case <-timer.C:
			}
		}
		r.Seq++
		state := cloneMap(frame.State)
		if _, present := state["protocol_version"]; !present {
			state["protocol_version"] = 1
		}
		state["sequence"] = r.Seq
		state["clock"] = map[string]any{"mode": "realtime"}
		if err := r.Client.SendOnly(ctx, map[string]any{"cmd": "set_state", "request_id": fmt.Sprintf("go-rep-%d", r.Seq), "state": state}); err != nil {
			return index, err
		}
	}
	if logf != nil {
		logf("replayed %d frames over ~%.0f ms", len(recording.Frames), recording.Frames[len(recording.Frames)-1].TMS-base)
	}
	return len(recording.Frames), nil
}

func cloneMap(input map[string]any) map[string]any {
	output := make(map[string]any, len(input))
	for key, value := range input {
		output[key] = cloneValue(value)
	}
	return output
}

func cloneValue(value any) any {
	switch v := value.(type) {
	case map[string]any:
		return cloneMap(v)
	case []any:
		out := make([]any, len(v))
		for i, item := range v {
			out[i] = cloneValue(item)
		}
		return out
	default:
		return v
	}
}
