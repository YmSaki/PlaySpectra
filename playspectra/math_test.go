// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package playspectra

import (
	"math"
	"testing"
)

func closeVector(got, want []float64, tolerance float64) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if math.Abs(got[i]-want[i]) > tolerance {
			return false
		}
	}
	return true
}

func vectorLengthSquared(v []float64) float64 {
	var total float64
	for _, value := range v {
		total += value * value
	}
	return total
}

// These subtests map one-to-one to the 17 characterization cases in
// tools/playspectra_math_test.py.
func TestPythonMathCharacterization(t *testing.T) {
	const tolerance = 1e-9
	identity := []float64{0, 0, 0, 1}
	r2 := math.Sqrt(0.5)

	t.Run("lerp endpoints and midpoint", func(t *testing.T) {
		a, b := []float64{0, 0, 0}, []float64{2, 4, 6}
		for name, test := range map[string]struct {
			t    float64
			want []float64
		}{
			"start": {0, a}, "end": {1, b}, "midpoint": {0.5, []float64{1, 2, 3}},
		} {
			if got := Lerp3(a, b, test.t); !closeVector(got, test.want, tolerance) {
				t.Fatalf("%s: got %v, want %v", name, got, test.want)
			}
		}
	})
	t.Run("lerp extrapolates linearly", func(t *testing.T) {
		if got, want := Lerp3([]float64{0, 0, 0}, []float64{1, 0, 0}, 2), []float64{2, 0, 0}; !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})

	t.Run("quat mul identity is neutral", func(t *testing.T) {
		q := QuatNorm([]float64{0.3, -0.5, 0.2, 0.8})
		if !closeVector(QuatMul(identity, q), q, tolerance) || !closeVector(QuatMul(q, identity), q, tolerance) {
			t.Fatalf("identity was not neutral for %v", q)
		}
	})
	t.Run("quat mul i times j is k", func(t *testing.T) {
		if got, want := QuatMul([]float64{1, 0, 0, 0}, []float64{0, 1, 0, 0}), []float64{0, 0, 1, 0}; !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})
	t.Run("quat mul is non-commutative", func(t *testing.T) {
		if got, want := QuatMul([]float64{0, 1, 0, 0}, []float64{1, 0, 0, 0}), []float64{0, 0, -1, 0}; !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})

	t.Run("quat yaw zero is identity", func(t *testing.T) {
		if got := QuatYaw(0); !closeVector(got, identity, tolerance) {
			t.Fatalf("got %v, want %v", got, identity)
		}
	})
	t.Run("quat yaw half pi", func(t *testing.T) {
		if got, want := QuatYaw(math.Pi/2), []float64{0, r2, 0, r2}; !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})
	t.Run("quat yaw pi", func(t *testing.T) {
		if got, want := QuatYaw(math.Pi), []float64{0, 1, 0, 0}; !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})
	t.Run("quat yaw composes", func(t *testing.T) {
		if got, want := QuatMul(QuatYaw(math.Pi/4), QuatYaw(math.Pi/4)), QuatYaw(math.Pi/2); !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})

	t.Run("quat norm scales to unit", func(t *testing.T) {
		if got, want := QuatNorm([]float64{0, 0, 0, 2}), identity; !closeVector(got, want, tolerance) {
			t.Fatalf("got %v, want %v", got, want)
		}
		if got := vectorLengthSquared(QuatNorm([]float64{1, 2, 3, 4})); math.Abs(got-1) > tolerance {
			t.Fatalf("length squared = %v", got)
		}
	})
	t.Run("quat norm zero stays zero", func(t *testing.T) {
		zero := []float64{0, 0, 0, 0}
		if got := QuatNorm(zero); !closeVector(got, zero, tolerance) {
			t.Fatalf("got %v, want %v", got, zero)
		}
	})

	t.Run("slerp same endpoints", func(t *testing.T) {
		q := QuatYaw(0.7)
		if got := Slerp(q, q, 0.5); !closeVector(got, q, 1e-6) {
			t.Fatalf("got %v, want %v", got, q)
		}
	})
	t.Run("slerp endpoints", func(t *testing.T) {
		q1 := QuatYaw(math.Pi / 2)
		if got := Slerp(identity, q1, 0); !closeVector(got, identity, 1e-6) {
			t.Fatalf("start = %v", got)
		}
		if got := Slerp(identity, q1, 1); !closeVector(got, q1, 1e-6) {
			t.Fatalf("end = %v", got)
		}
	})
	t.Run("slerp midpoint is half angle", func(t *testing.T) {
		if got, want := Slerp(identity, QuatYaw(math.Pi/2), 0.5), QuatYaw(math.Pi/4); !closeVector(got, want, 1e-6) {
			t.Fatalf("got %v, want %v", got, want)
		}
	})
	t.Run("slerp result is unit length", func(t *testing.T) {
		got := vectorLengthSquared(Slerp(QuatYaw(0.2), QuatYaw(2.5), 0.37))
		if math.Abs(got-1) > tolerance {
			t.Fatalf("length squared = %v", got)
		}
	})
	t.Run("slerp takes antipodal short path", func(t *testing.T) {
		q := QuatYaw(0.6)
		negative := []float64{-q[0], -q[1], -q[2], -q[3]}
		got := Slerp(q, negative, 0.5)
		if !closeVector(got, q, 1e-6) && !closeVector(got, negative, 1e-6) {
			t.Fatalf("got %v, want %v or %v", got, q, negative)
		}
	})
	t.Run("slerp near-colinear uses linear branch", func(t *testing.T) {
		got := vectorLengthSquared(Slerp(QuatYaw(0.010), QuatYaw(0.011), 0.5))
		if math.Abs(got-1) > tolerance {
			t.Fatalf("length squared = %v", got)
		}
	})
}
