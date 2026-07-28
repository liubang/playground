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

package app

import (
	"context"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func testQuestion() domain.Question {
	return domain.Question{
		Text: "which strategy?",
		Options: []domain.QuestionOption{
			{Label: "in-place"},
			{Label: "dual-write"},
		},
	}
}

func TestChannelQuestionerAskResolve(t *testing.T) {
	var published []domain.Question
	q := NewChannelQuestioner(func(question domain.Question) {
		published = append(published, question)
	})

	type result struct {
		answer domain.QuestionAnswer
		err    error
	}
	resultCh := make(chan result, 1)
	go func() {
		answer, err := q.Ask(context.Background(), testQuestion())
		resultCh <- result{answer, err}
	}()

	deadline := time.Now().Add(2 * time.Second)
	for len(q.PendingQuestions()) == 0 {
		if time.Now().After(deadline) {
			t.Fatal("question never became pending")
		}
		time.Sleep(5 * time.Millisecond)
	}
	pending := q.PendingQuestions()
	if len(pending) != 1 {
		t.Fatalf("pending = %v, want 1 question", pending)
	}

	if !q.Resolve(pending[0], domain.QuestionAnswer{Selected: []string{"dual-write"}}) {
		t.Fatal("Resolve rejected the pending question")
	}
	select {
	case res := <-resultCh:
		if res.err != nil {
			t.Fatalf("Ask error = %v", res.err)
		}
		if len(res.answer.Selected) != 1 || res.answer.Selected[0] != "dual-write" {
			t.Fatalf("answer = %+v", res.answer)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Ask did not return after Resolve")
	}

	if len(published) != 1 || published[0].Text != "which strategy?" {
		t.Fatalf("published = %+v", published)
	}
	// The ID is assigned by the questioner when the caller leaves it zero.
	if published[0].ID.IsZero() {
		t.Fatal("question ID must be assigned at ask time")
	}

	// A second resolve on the same ID is a no-op.
	if q.Resolve(pending[0], domain.QuestionAnswer{Skipped: true}) {
		t.Fatal("duplicate Resolve must be rejected")
	}
}

func TestChannelQuestionerContextCancel(t *testing.T) {
	q := NewChannelQuestioner(nil)
	ctx, cancel := context.WithCancel(context.Background())
	resultCh := make(chan domain.QuestionAnswer, 1)
	go func() {
		answer, _ := q.Ask(ctx, testQuestion())
		resultCh <- answer
	}()
	cancel()
	select {
	case answer := <-resultCh:
		if !answer.Skipped {
			t.Fatalf("answer = %+v, want skipped on cancel", answer)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Ask did not return after cancel")
	}
}

func TestChannelQuestionerSkipAllIsIdempotent(t *testing.T) {
	q := NewChannelQuestioner(nil)
	q.SkipAll()
	q.SkipAll()
	answer, err := q.Ask(context.Background(), testQuestion())
	if err == nil {
		t.Fatal("Ask after shutdown must fail")
	}
	if !answer.Skipped {
		t.Fatalf("answer = %+v, want skipped after shutdown", answer)
	}
}

func TestChannelQuestionerRejectsInvalidQuestion(t *testing.T) {
	q := NewChannelQuestioner(nil)
	if _, err := q.Ask(context.Background(), domain.Question{}); err == nil {
		t.Fatal("empty question must be rejected")
	}
}
