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
// Created: 2026/09/28

package toolkit

import (
	"encoding/json"
	"testing"
)

func TestRepairJSONRejectsAlreadyValid(t *testing.T) {
	for _, raw := range []string{
		`{"path":"a.go"}`,
		`  {"path":"a.go"}  `,
		`"just a string"`,
		`42`,
	} {
		if _, ok := RepairJSON(raw); ok {
			t.Fatalf("RepairJSON(%q) = ok, want not-ok for already-valid JSON", raw)
		}
	}
}

func TestRepairJSONUnquotedStringValue(t *testing.T) {
	// Real deepseek-v4-flash failure shape (unquoted CJK string value):
	// the model mirrored the goal-action's objective field into a plan
	// call and emitted its value without quotes.
	raw := `{"action": "plan", "objective": 深入分析代码库并产出结构化报告, "plan": [{"goal": "摸清仓库全景", "status": "in_progress"}, {"goal": "梳理工具集", "status": "pending"}]}`
	repaired, ok := RepairJSON(raw)
	if !ok {
		t.Fatalf("RepairJSON failed on unquoted CJK value")
	}
	var args struct {
		Action string `json:"action"`
		Plan   []struct {
			Goal   string `json:"goal"`
			Status string `json:"status"`
		} `json:"plan"`
	}
	if err := json.Unmarshal(repaired, &args); err != nil {
		t.Fatalf("repaired payload does not decode: %v (%s)", err, repaired)
	}
	if args.Action != "plan" || len(args.Plan) != 2 || args.Plan[0].Status != "in_progress" {
		t.Fatalf("repaired payload lost structure: %+v", args)
	}
}

func TestRepairJSONLiteralNewlineInString(t *testing.T) {
	// Real glm-5.2 failure: pretty-printed arguments with a literal
	// newline inside a string value.
	raw := "{\n  \"path\": \"test.go\n\"}"
	repaired, ok := RepairJSON(raw)
	if !ok {
		t.Fatalf("RepairJSON failed on literal newline in string")
	}
	var args struct {
		Path string `json:"path"`
	}
	if err := json.Unmarshal(repaired, &args); err != nil {
		t.Fatalf("repaired payload does not decode: %v (%s)", err, repaired)
	}
	if args.Path == "" {
		t.Fatalf("repaired payload lost the path value: %s", repaired)
	}
}

func TestRepairJSONTruncatedTail(t *testing.T) {
	repaired, ok := RepairJSON(`{"action": "plan", "plan": [{"goal": "a", "status": "in_progress"}`)
	if !ok {
		t.Fatalf("RepairJSON failed on truncated payload")
	}
	var args struct {
		Action string `json:"action"`
	}
	if err := json.Unmarshal(repaired, &args); err != nil || args.Action != "plan" {
		t.Fatalf("repaired payload = %s, err = %v", repaired, err)
	}
}

func TestRepairJSONGarbageStaysRejected(t *testing.T) {
	for _, raw := range []string{"", "   ", "not json at all {{{"} {
		if repaired, ok := RepairJSON(raw); ok && !json.Valid(repaired) {
			t.Fatalf("RepairJSON(%q) returned invalid JSON %q", raw, repaired)
		}
	}
}
