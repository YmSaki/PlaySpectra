// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package verify

import (
	"context"
	"math"
	"time"
)

func pose(position []float64) map[string]any {
	return map[string]any{
		"position": position, "orientation": []float64{0, 0, 0, 1},
		"relation_flags": map[string]any{
			"position_valid": true, "orientation_valid": true,
			"position_tracked": true, "orientation_tracked": true,
		},
	}
}

func requestOK(response map[string]any) bool { value, _ := response["ok"].(bool); return value }
func boolean(value any) bool                 { result, _ := value.(bool); return result }
func text(value any) string                  { result, _ := value.(string); return result }

func number(value any) (float64, bool) {
	switch result := value.(type) {
	case float64:
		return result, true
	case int:
		return float64(result), true
	case int64:
		return float64(result), true
	default:
		return 0, false
	}
}

func near(value any, want, tolerance float64) bool {
	actual, ok := number(value)
	return ok && math.Abs(actual-want) < tolerance
}

func resolve(root any, keys ...any) any {
	current := root
	for _, key := range keys {
		switch value := current.(type) {
		case map[string]any:
			name, ok := key.(string)
			if !ok {
				return nil
			}
			current = value[name]
		case []any:
			index, ok := key.(int)
			if !ok || index < 0 || index >= len(value) {
				return nil
			}
			current = value[index]
		case []float64:
			index, ok := key.(int)
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

func sleep(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
