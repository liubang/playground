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
// Created: 2026/07/27

package agent

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

type stubQuestioner struct {
	answer domain.QuestionAnswer
	err    error
	asked  []domain.Question
}

func (s *stubQuestioner) Ask(_ context.Context, q domain.Question) (domain.QuestionAnswer, error) {
	s.asked = append(s.asked, q)
	return s.answer, s.err
}

func newAskUserTool(t *testing.T, questioner domain.Questioner) *AskUserTool {
	t.Helper()
	tool, err := NewAskUserTool(questioner)
	if err != nil {
		t.Fatalf("NewAskUserTool: %v", err)
	}
	return tool
}

func askUserCall(t *testing.T, args string) domain.ToolCall {
	t.Helper()
	return domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "ask_user",
		Arguments: json.RawMessage(args),
	}
}

func TestAskUserToolRiskIsR0(t *testing.T) {
	tool := newAskUserTool(t, &stubQuestioner{})
	def := tool.Definition()
	if got := def.Risk(); got != domain.R0 {
		t.Fatalf("ask_user risk = %v, want R0 (a question must never need approval)", got)
	}
}

func TestAskUserToolAnswerFlow(t *testing.T) {
	questioner := &stubQuestioner{answer: domain.QuestionAnswer{Selected: []string{"dual-write"}, CustomText: "but stage it"}}
	tool := newAskUserTool(t, questioner)

	prepared, err := tool.Prepare(context.Background(), askUserCall(t, `{"question":"which strategy?","options":[{"label":"in-place","description":"fast but risky"},{"label":"dual-write"}],"allow_multiple":false}`))
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("result = %+v", result)
	}

	var payload struct {
		Answered   bool     `json:"answered"`
		Selected   []string `json:"selected"`
		CustomText string   `json:"custom_text"`
		Note       string   `json:"note"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("result content: %v", err)
	}
	if !payload.Answered || len(payload.Selected) != 1 || payload.Selected[0] != "dual-write" || payload.CustomText != "but stage it" {
		t.Fatalf("payload = %+v", payload)
	}

	// The questioner saw a validated, ID-assigned question.
	if len(questioner.asked) != 1 {
		t.Fatalf("asked = %d, want 1", len(questioner.asked))
	}
	asked := questioner.asked[0]
	if asked.ID.IsZero() || asked.Text != "which strategy?" || len(asked.Options) != 2 {
		t.Fatalf("asked = %+v", asked)
	}
	if asked.Options[0].Description != "fast but risky" {
		t.Fatalf("option description lost: %+v", asked.Options[0])
	}
}

func TestAskUserToolSkippedAnswer(t *testing.T) {
	tool := newAskUserTool(t, &stubQuestioner{answer: domain.QuestionAnswer{Skipped: true}})
	prepared, err := tool.Prepare(context.Background(), askUserCall(t, `{"question":"go on?","options":[{"label":"yes"},{"label":"no"}]}`))
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("a skip is a successful answer, not an error: %+v", result)
	}
	var payload struct {
		Answered bool   `json:"answered"`
		Note     string `json:"note"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("result content: %v", err)
	}
	if payload.Answered || !strings.Contains(payload.Note, "best judgment") {
		t.Fatalf("payload = %+v, want skip guidance", payload)
	}
}

func TestAskUserToolPrepareValidation(t *testing.T) {
	tool := newAskUserTool(t, &stubQuestioner{})
	cases := []struct {
		name string
		args string
	}{
		{"missing question", `{"options":[{"label":"a"}]}`},
		{"missing options", `{"question":"q?"}`},
		{"empty options", `{"question":"q?","options":[]}`},
		{"empty label", `{"question":"q?","options":[{"label":""}]}`},
		{"duplicate labels", `{"question":"q?","options":[{"label":"a"},{"label":"a"}]}`},
		{"unknown field", `{"question":"q?","options":[{"label":"a"}],"bogus":1}`},
		{"malformed json", `{"question"`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := tool.Prepare(context.Background(), askUserCall(t, tc.args)); err == nil {
				t.Fatalf("Prepare(%s) should fail", tc.args)
			}
		})
	}
}

func TestAskUserToolQuestionerError(t *testing.T) {
	tool := newAskUserTool(t, &stubQuestioner{err: context.Canceled})
	prepared, err := tool.Prepare(context.Background(), askUserCall(t, `{"question":"q?","options":[{"label":"a"}]}`))
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError || result.Error == nil {
		t.Fatalf("result = %+v, want error", result)
	}
}

func TestAutonomousQuestionerSkips(t *testing.T) {
	answer, err := domain.AutonomousQuestioner{}.Ask(context.Background(), domain.Question{})
	if err != nil {
		t.Fatalf("AutonomousQuestioner must never fail: %v", err)
	}
	if !answer.Skipped {
		t.Fatalf("answer = %+v, want skipped", answer)
	}
}
