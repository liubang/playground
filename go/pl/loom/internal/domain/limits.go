// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/22 21:10

package domain

import "time"

// Limits constrains the resources a Run can consume.
type Limits struct {
	MaxTurns            int           `json:"max_turns"`
	MaxToolCalls        int           `json:"max_tool_calls"`
	MaxParallelTools    int           `json:"max_parallel_tools"`
	MaxInputTokens      int64         `json:"max_input_tokens"`
	MaxOutputTokens     int64         `json:"max_output_tokens"`
	MaxEstimatedCostUSD float64       `json:"max_estimated_cost_usd"`
	MaxWallTime         time.Duration `json:"max_wall_time"`
	MaxToolOutputBytes  int64         `json:"max_tool_output_bytes"`
	MaxArtifactBytes    int64         `json:"max_artifact_bytes"`
	MaxRepeatedActions  int           `json:"max_repeated_actions"`
}

// DefaultLimits returns the standard limits.
func DefaultLimits() Limits {
	return Limits{
		MaxTurns:            50,
		MaxToolCalls:        200,
		MaxParallelTools:    4,
		MaxInputTokens:      200_000,
		MaxOutputTokens:     16_384,
		MaxEstimatedCostUSD: 5.0,
		MaxWallTime:         30 * time.Minute,
		MaxToolOutputBytes:  64 * 1024,
		MaxArtifactBytes:    100 * 1024 * 1024,
		MaxRepeatedActions:  3,
	}
}

// Usage tracks accumulated resource consumption against Limits.
type Usage struct {
	Turns        int
	ToolCalls    int
	InputTokens  int64
	OutputTokens int64
	CostUSD      float64
	WallTime     time.Duration
}

// CheckResult reports soft/hard threshold breaches.
type CheckResult struct {
	SoftBreaches []string
	HardBreaches []string
}

// HasSoft reports whether any soft threshold is breached.
func (c CheckResult) HasSoft() bool { return len(c.SoftBreaches) > 0 }

// HasHard reports whether any hard threshold is breached.
func (c CheckResult) HasHard() bool { return len(c.HardBreaches) > 0 }

// Check evaluates current usage against limits.
// Soft = 80% of limit (prompt model to converge/compress).
// Hard = 100% of limit (must terminate).
func (u Usage) Check(l Limits) CheckResult {
	var res CheckResult
	soft := func(name string, cur, limit float64) {
		if limit <= 0 {
			return
		}
		ratio := cur / limit
		if ratio >= 1.0 {
			res.HardBreaches = append(res.HardBreaches, name)
		} else if ratio >= 0.8 {
			res.SoftBreaches = append(res.SoftBreaches, name)
		}
	}
	soft("turns", float64(u.Turns), float64(l.MaxTurns))
	soft("tool_calls", float64(u.ToolCalls), float64(l.MaxToolCalls))
	soft("input_tokens", float64(u.InputTokens), float64(l.MaxInputTokens))
	soft("output_tokens", float64(u.OutputTokens), float64(l.MaxOutputTokens))
	soft("cost_usd", u.CostUSD, l.MaxEstimatedCostUSD)
	soft("wall_time", float64(u.WallTime), float64(l.MaxWallTime))
	return res
}
