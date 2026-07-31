package verify

import (
	"context"
	"fmt"
	"math"

	"github.com/YmSaki/PlaySpectra/playspectra"
	"github.com/YmSaki/PlaySpectra/protocol"
)

func Coupling(ctx context.Context, operateAddress, captureAddress string, targetZ, tolerance float64) (Report, error) {
	report := NewReport("runtime-coupling")
	if targetZ == 0 {
		targetZ = -2.5
	}
	if tolerance <= 0 {
		tolerance = 0.3
	}
	capture, err := protocol.Dial(ctx, captureAddress)
	if err != nil {
		return report, err
	}
	defer capture.Close()
	if _, err := capture.RequestLine(ctx, map[string]any{"cmd": "head_clear"}); err != nil {
		return report, err
	}
	if !sleep(ctx, 600_000_000) {
		return report, ctx.Err()
	}
	baseline, rawBaseline, err := viewPose(ctx, capture)
	if err != nil {
		return report, err
	}
	if baseline == nil {
		return report, fmt.Errorf("no baseline view: %v", rawBaseline)
	}
	operate, err := protocol.Dial(ctx, operateAddress)
	if err != nil {
		return report, err
	}
	defer operate.Close()
	server := playspectra.NewServer(operate, playspectra.WithLogger(func(string, ...any) {}))
	if err := server.Hello(ctx, "coupling"); err != nil {
		return report, err
	}
	if err := server.MoveHead(ctx, map[string]any{"position": []float64{0, 1.6, targetZ}}, 300); err != nil {
		return report, err
	}
	if !sleep(ctx, 1_000_000_000) {
		return report, ctx.Err()
	}
	after, rawAfter, err := viewPose(ctx, capture)
	if err != nil {
		return report, err
	}
	if after == nil {
		return report, fmt.Errorf("no post-move view: %v", rawAfter)
	}
	dx := math.Abs(numberOrZero(after["x"]) - numberOrZero(baseline["x"]))
	dy := math.Abs(numberOrZero(after["y"]) - numberOrZero(baseline["y"]))
	dz := numberOrZero(after["z"]) - numberOrZero(baseline["z"])
	expectedDZ := targetZ - numberOrZero(baseline["z"])
	report.Add("runtime HMD move reaches the app's xrLocateViews (z tracks command)", math.Abs(dz-expectedDZ) < tolerance, fmt.Sprintf("dz=%.3f expected=%.3f", dz, expectedDZ))
	report.Add("pure-z command does not drift the view sideways", dx < tolerance && dy < tolerance, fmt.Sprintf("dx=%.3f dy=%.3f", dx, dy))
	return report, nil
}

func viewPose(ctx context.Context, client *protocol.Client) (map[string]any, map[string]any, error) {
	response, err := client.RequestLine(ctx, map[string]any{"cmd": "view"})
	if err != nil {
		return nil, nil, err
	}
	views, _ := response["views"].([]any)
	if len(views) == 0 {
		return nil, response, nil
	}
	view, _ := views[0].(map[string]any)
	pose, _ := view["pose"].(map[string]any)
	return pose, response, nil
}

func numberOrZero(value any) float64 { result, _ := number(value); return result }
