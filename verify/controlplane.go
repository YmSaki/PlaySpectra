// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"context"
	"fmt"
	"math"
	"time"

	"github.com/YmSaki/PlaySpectra/playspectra"
	"github.com/YmSaki/PlaySpectra/protocol"
)

func Server(ctx context.Context, address string, rate float64) (Report, error) {
	report := NewReport("server")
	client, err := protocol.Dial(ctx, address)
	if err != nil {
		return report, err
	}
	defer client.Close()
	server := playspectra.NewServer(client, playspectra.WithRate(rate), playspectra.WithLogger(func(string, ...any) {}))
	if err := server.Hello(ctx, "writer"); err != nil {
		return report, err
	}
	getState := func() (map[string]any, error) { return server.GetState(ctx) }
	state, err := getState()
	if err != nil {
		return report, err
	}
	baseZ := resolve(state, "hmd", "head", "position", 2)
	report.Add("seeded at builder head z~0", near(baseZ, 0, 0.001), fmt.Sprintf("z=%v", baseZ))
	if err := server.MoveHead(ctx, map[string]any{"position": []float64{0, 1.6, -1}}, 300); err != nil {
		return report, err
	}
	state, err = getState()
	if err != nil {
		return report, err
	}
	z := resolve(state, "hmd", "head", "position", 2)
	report.Add("move_head -> z~-1.0", near(z, -1, 0.01), fmt.Sprintf("z=%v", z))
	if err := server.Look(ctx, 90, 300); err != nil {
		return report, err
	}
	state, err = getState()
	if err != nil {
		return report, err
	}
	qy := resolve(state, "hmd", "head", "orientation", 1)
	report.Add("look 90deg -> quat.y ~ sin(45)=0.707", near(qy, 0.7071, 0.05), fmt.Sprintf("qy=%v", qy))
	if err := server.WalkForward(ctx, 1, 200, "left"); err != nil {
		return report, err
	}
	state, err = getState()
	if err != nil {
		return report, err
	}
	stick := resolve(state, "left", "inputs", "/input/thumbstick/y")
	report.Add("walk_forward releases stick to 0 at end", near(stick, 0, 0.001), fmt.Sprintf("ty=%v", stick))
	if err := server.Press(ctx, "right", "a", 80); err != nil {
		return report, err
	}
	state, err = getState()
	if err != nil {
		return report, err
	}
	click := resolve(state, "right", "inputs", "/button/a/click")
	report.Add("press releases button (a/click false at end)", click == false, fmt.Sprintf("a=%v", click))
	if err := server.MoveHead(ctx, map[string]any{"position": []float64{0, 1.6, -2}}, 200); err != nil {
		return report, err
	}
	met, err := server.WaitFor(ctx, []any{"hmd", "head", "position", 2}, "near", -2.0, 0.01, 1500, 40, "")
	if err != nil {
		return report, err
	}
	report.Add("wait_for confirms move_head -> z~-2 (auto-wait)", met, fmt.Sprintf("met=%v", met))
	met, err = server.WaitFor(ctx, []any{"hmd", "head", "position", 2}, "near", 999.0, 0.001, 250, 40, "")
	if err != nil {
		return report, err
	}
	report.Add("wait_for times out on impossible condition (no hang)", !met, fmt.Sprintf("returned=%v", met))
	asserted, err := server.AssertState(ctx, []any{"hmd", "head", "position", 2}, "near", -2.0, 0.01, "", 500, 50)
	if err != nil {
		return report, err
	}
	report.Add("retrying assert passes when condition holds", asserted, fmt.Sprintf("ok=%v", asserted))
	if err := server.Reset(ctx); err != nil {
		return report, err
	}
	state, err = getState()
	if err != nil {
		return report, err
	}
	resetZ := resolve(state, "hmd", "head", "position", 2)
	report.Add("reset -> head back to z~0", near(resetZ, 0, 0.001), fmt.Sprintf("z=%v", resetZ))
	return report, nil
}

func Recording(ctx context.Context, address string, rate float64) (Report, error) {
	report := NewReport("record-replay")
	writer, err := protocol.Dial(ctx, address)
	if err != nil {
		return report, err
	}
	defer writer.Close()
	observer, err := protocol.Dial(ctx, address)
	if err != nil {
		return report, err
	}
	defer observer.Close()
	server := playspectra.NewServer(writer, playspectra.WithRate(rate), playspectra.WithLogger(func(string, ...any) {}))
	recorder := playspectra.NewRecorder(observer, rate)
	if err := server.Hello(ctx, "writer"); err != nil {
		return report, err
	}
	if err := recorder.Hello(ctx); err != nil {
		return report, err
	}
	started := time.Now()
	recordDone := make(chan error, 1)
	go func() {
		recordDone <- recorder.Run(ctx, started)
	}()
	if err := server.MoveHead(ctx, map[string]any{"position": []float64{0, 1.6, -2}}, 400); err != nil {
		recorder.Stop()
		<-recordDone
		return report, err
	}
	if err := server.Look(ctx, 30, 200); err != nil {
		recorder.Stop()
		<-recordDone
		return report, err
	}
	sleep(ctx, 100*time.Millisecond)
	recorder.Stop()
	if err := <-recordDone; err != nil {
		return report, err
	}
	frames := append([]playspectra.RecordingFrame(nil), recorder.Frames...)
	zValues := []float64{}
	for _, frame := range frames {
		if z, ok := number(resolve(frame.State, "hmd", "head", "position", 2)); ok {
			zValues = append(zValues, z)
		}
	}
	report.Add("recorded frames > 0", len(frames) > 0, fmt.Sprintf("frames=%d", len(frames)))
	minimum := math.Inf(1)
	for _, z := range zValues {
		minimum = math.Min(minimum, z)
	}
	report.Add("trajectory captured (min head z <= -1.8)", len(zValues) > 0 && minimum <= -1.8, fmt.Sprintf("min_z=%v", minimum))
	firstZ := math.Inf(1)
	if len(zValues) > 0 {
		firstZ = zValues[0]
	}
	report.Add("trajectory starts near builder z~0", len(zValues) > 0 && math.Abs(firstZ) < 0.3, fmt.Sprintf("z0=%v", firstZ))
	if err := server.Reset(ctx); err != nil {
		return report, err
	}
	state, err := server.GetState(ctx)
	if err != nil {
		return report, err
	}
	z := resolve(state, "hmd", "head", "position", 2)
	report.Add("reset before replay -> z~0", near(z, 0, 0.001), fmt.Sprintf("z=%v", z))
	replayer := playspectra.NewReplayer(writer)
	if _, err := replayer.Play(ctx, playspectra.Recording{Frames: frames}, nil); err != nil {
		return report, err
	}
	state, err = server.GetState(ctx)
	if err != nil {
		return report, err
	}
	z = resolve(state, "hmd", "head", "position", 2)
	report.Add("replay reproduced final head z (~-2.0)", near(z, -2, 0.15), fmt.Sprintf("z=%v", z))
	return report, nil
}
