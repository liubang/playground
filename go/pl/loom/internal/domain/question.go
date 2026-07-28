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

package domain

import (
	"context"
	"fmt"
)

// QuestionOption is one selectable choice in a Question.
type QuestionOption struct {
	Label       string `json:"label"`
	Description string `json:"description,omitempty"`
}

// Question is a model-initiated request for user input in the middle of
// tool execution: the model is uncertain between several reasonable
// directions and asks instead of guessing. The ID is assigned by the
// Questioner at ask time (zero before that).
type Question struct {
	ID            EventID          `json:"id"`
	Text          string           `json:"text"`
	Options       []QuestionOption `json:"options"`
	AllowMultiple bool             `json:"allow_multiple,omitempty"`
}

// Validate checks the question is well-formed.
func (q Question) Validate() error {
	if q.Text == "" {
		return fmt.Errorf("question text required")
	}
	if len(q.Options) == 0 {
		return fmt.Errorf("question must have at least one option")
	}
	seen := make(map[string]bool, len(q.Options))
	for i, opt := range q.Options {
		if opt.Label == "" {
			return fmt.Errorf("option[%d]: label required", i)
		}
		if seen[opt.Label] {
			return fmt.Errorf("option[%d]: duplicate label %q", i, opt.Label)
		}
		seen[opt.Label] = true
	}
	return nil
}

// QuestionAnswer is the user's resolution of a Question. Skipped marks an
// unanswered question (user dismissed it, or no interactive frontend
// exists); the model is then expected to proceed with its best judgment.
type QuestionAnswer struct {
	Selected   []string `json:"selected,omitempty"`
	CustomText string   `json:"custom_text,omitempty"`
	Skipped    bool     `json:"skipped,omitempty"`
}

// Questioner resolves model questions by asking the user, mirroring how
// Approver resolves permission decisions. Implementations block until the
// question is answered, skipped, or the context is cancelled.
type Questioner interface {
	Ask(ctx context.Context, q Question) (QuestionAnswer, error)
}

// AutonomousQuestioner is the Questioner for non-interactive environments
// (headless runs, CI): every question is immediately skipped so the model
// proceeds autonomously instead of blocking on input that can never come.
type AutonomousQuestioner struct{}

// Ask implements Questioner.
func (AutonomousQuestioner) Ask(_ context.Context, _ Question) (QuestionAnswer, error) {
	return QuestionAnswer{Skipped: true}, nil
}
