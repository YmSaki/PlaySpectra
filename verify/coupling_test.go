// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"context"
	"strings"
	"testing"
)

// TestCouplingHonorsTheRequestedTargetAndTolerance guards the probe's two
// parameters against being substituted for defaults. Both checks in the report
// are derived from targetZ, so a substituted target stays self-consistent and
// the report passes either way; the discriminating observation is where the
// adapter's head actually ended up.
func TestCouplingHonorsTheRequestedTargetAndTolerance(t *testing.T) {
	t.Run("a target z of zero is a target, not a missing value", func(t *testing.T) {
		adapter := startFixtureAdapter(t)
		capture := startFixtureCapture(t, adapter)
		report, err := Coupling(context.Background(), adapter.address(), capture.address(), 0, 0.3)
		if err != nil {
			t.Fatal(err)
		}
		if z := headZ(t, adapter); z != 0 {
			t.Fatalf("head ended at z=%v, want the commanded 0", z)
		}
		if !report.OK() {
			t.Fatalf("report = %+v", report)
		}
	})

	t.Run("a target z is driven all the way to the adapter", func(t *testing.T) {
		adapter := startFixtureAdapter(t)
		capture := startFixtureCapture(t, adapter)
		if _, err := Coupling(context.Background(), adapter.address(), capture.address(), -1.25, 0.3); err != nil {
			t.Fatal(err)
		}
		if z := headZ(t, adapter); z != -1.25 {
			t.Fatalf("head ended at z=%v, want the commanded -1.25", z)
		}
	})

	// The fixture moves the view exactly onto the command, so a zero tolerance
	// is the demand for an exact match rather than an impossible bar: the
	// comparison is inclusive, and an exact move is what the fixture delivers.
	t.Run("a zero tolerance demands an exact match", func(t *testing.T) {
		adapter := startFixtureAdapter(t)
		capture := startFixtureCapture(t, adapter)
		report, err := Coupling(context.Background(), adapter.address(), capture.address(), -2.5, 0)
		if err != nil {
			t.Fatal(err)
		}
		if report.Passed() != 2 || len(report.Checks) != 2 {
			t.Fatalf("zero tolerance passed %d/%d checks: %+v", report.Passed(), len(report.Checks), report)
		}
	})

	t.Run("the smallest positive tolerance still accepts an exact move", func(t *testing.T) {
		adapter := startFixtureAdapter(t)
		capture := startFixtureCapture(t, adapter)
		report, err := Coupling(context.Background(), adapter.address(), capture.address(), -2.5, 1e-9)
		if err != nil {
			t.Fatal(err)
		}
		if !report.OK() {
			t.Fatalf("report = %+v", report)
		}
	})
}

// TestCouplingRejectsAnIncompletePose covers the reading a target of zero made
// indistinguishable from a real one: a pose without x, y or z used to read as
// that axis being at the origin, so a runtime that never reported a position
// scored a passing "the head moved to z=0" instead of an error.
func TestCouplingRejectsAnIncompletePose(t *testing.T) {
	reads := []struct {
		name string
		call int
	}{{"baseline", 1}, {"post-move", 2}}
	for _, read := range reads {
		for _, axis := range []string{"x", "y", "z"} {
			t.Run(read.name+" pose without "+axis, func(t *testing.T) {
				adapter := startFixtureAdapter(t)
				capture := startFixtureCapture(t, adapter, dropPoseAxis(read.call, axis))
				report, err := Coupling(context.Background(), adapter.address(), capture.address(), 0, 0.3)
				if err == nil {
					t.Fatalf("a %s pose without %s was accepted: %+v", read.name, axis, report)
				}
				if !strings.Contains(err.Error(), axis) {
					t.Fatalf("error does not name the missing field %q: %v", axis, err)
				}
				if len(report.Checks) != 0 {
					t.Fatalf("checks were scored from an incomplete pose: %+v", report)
				}
			})
		}
	}
}

func headZ(t *testing.T, adapter *fixtureAdapter) float64 {
	t.Helper()
	position := adapter.headPosition()
	if len(position) != 3 {
		t.Fatalf("head position = %v", position)
	}
	z, ok := number(position[2])
	if !ok {
		t.Fatalf("head z is not a number: %v", position[2])
	}
	return z
}
