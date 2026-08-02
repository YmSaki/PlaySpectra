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
	from, err := posePosition(baseline, "baseline")
	if err != nil {
		return report, err
	}
	to, err := posePosition(after, "post-move")
	if err != nil {
		return report, err
	}
	dx := math.Abs(to[0] - from[0])
	dy := math.Abs(to[1] - from[1])
	dz := to[2] - from[2]
	expectedDZ := targetZ - from[2]
	// The comparison is inclusive so that a tolerance of 0 means what it says:
	// the view must land exactly on the command, not on nothing at all.
	report.Add("runtime HMD move reaches the app's xrLocateViews (z tracks command)", math.Abs(dz-expectedDZ) <= tolerance, fmt.Sprintf("dz=%.3f expected=%.3f", dz, expectedDZ))
	report.Add("pure-z command does not drift the view sideways", dx <= tolerance && dy <= tolerance, fmt.Sprintf("dx=%.3f dy=%.3f", dx, dy))
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

// posePosition requires the runtime to have reported all three coordinates. A
// missing axis read as 0 would be indistinguishable from a head resting at the
// origin, which is a legal target, so an absent field is an error rather than a
// value the checks can be scored against.
func posePosition(pose map[string]any, label string) ([3]float64, error) {
	var position [3]float64
	for index, axis := range []string{"x", "y", "z"} {
		value, ok := number(pose[axis])
		if !ok {
			return position, fmt.Errorf("%s view pose has no %s: %v", label, axis, pose)
		}
		position[index] = value
	}
	return position, nil
}
