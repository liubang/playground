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
	"fmt"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ChannelQuestioner bridges model questions between the agent loop and an
// interactive frontend (TUI). It implements domain.Questioner with the same
// channel discipline as ChannelApprover: Ask registers a pending slot,
// publishes the question through the injected hook, and blocks until the
// frontend resolves it, the context is cancelled, or shutdown occurs.
type ChannelQuestioner struct {
	mu       sync.Mutex
	pending  map[domain.EventID]chan domain.QuestionAnswer
	done     chan struct{}
	doneOnce sync.Once
	publish  func(domain.Question)
}

// NewChannelQuestioner creates a questioner. publish is invoked with each
// newly registered question (typically emitting a runtime event); it must
// be non-blocking and may be nil in tests.
func NewChannelQuestioner(publish func(domain.Question)) *ChannelQuestioner {
	return &ChannelQuestioner{
		pending: make(map[domain.EventID]chan domain.QuestionAnswer),
		done:    make(chan struct{}),
		publish: publish,
	}
}

// BindPublish (re)binds the publish hook after construction — the bridge to
// the runtime event stream is only available once the controller exists.
func (q *ChannelQuestioner) BindPublish(publish func(domain.Question)) {
	q.mu.Lock()
	q.publish = publish
	q.mu.Unlock()
}

// Ask implements domain.Questioner. The question ID is assigned here when
// the caller left it zero.
func (q *ChannelQuestioner) Ask(ctx context.Context, question domain.Question) (domain.QuestionAnswer, error) {
	if question.ID.IsZero() {
		question.ID = domain.NewEventID()
	}
	if err := question.Validate(); err != nil {
		return domain.QuestionAnswer{}, err
	}

	resultCh := make(chan domain.QuestionAnswer, 1)
	q.mu.Lock()
	select {
	case <-q.done:
		q.mu.Unlock()
		return domain.QuestionAnswer{Skipped: true}, fmt.Errorf("questioner shut down")
	default:
	}
	q.pending[question.ID] = resultCh
	q.mu.Unlock()
	defer func() {
		q.mu.Lock()
		delete(q.pending, question.ID)
		q.mu.Unlock()
	}()

	q.mu.Lock()
	publish := q.publish
	q.mu.Unlock()
	if publish != nil {
		publish(question)
	}

	select {
	case answer := <-resultCh:
		return answer, nil
	case <-q.done:
		return domain.QuestionAnswer{Skipped: true}, fmt.Errorf("questioner shut down")
	case <-ctx.Done():
		return domain.QuestionAnswer{Skipped: true}, ctx.Err()
	}
}

// Resolve delivers an answer to the pending question, returning false when
// the question is unknown or already resolved (one-shot semantics).
func (q *ChannelQuestioner) Resolve(id domain.EventID, answer domain.QuestionAnswer) bool {
	q.mu.Lock()
	resultCh, ok := q.pending[id]
	if ok {
		delete(q.pending, id)
	}
	q.mu.Unlock()
	if !ok {
		return false
	}
	resultCh <- answer
	return true
}

// SkipAll skips every pending question and prevents future asks. It is
// idempotent and safe to call during concurrent shutdown paths.
func (q *ChannelQuestioner) SkipAll() {
	q.doneOnce.Do(func() { close(q.done) })
}

// PendingQuestions returns the IDs of all unanswered questions.
func (q *ChannelQuestioner) PendingQuestions() []domain.EventID {
	q.mu.Lock()
	defer q.mu.Unlock()
	out := make([]domain.EventID, 0, len(q.pending))
	for id := range q.pending {
		out = append(out, id)
	}
	return out
}
