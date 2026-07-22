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

package fakes

import (
	"context"
	"fmt"
	"io"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ScriptEntry defines one model response in a FakeModel script.
type ScriptEntry struct {
	Text       string
	ToolCalls  []domain.ToolCall
	StopReason domain.StopReason
	UsageIn    int64
	UsageOut   int64
	Error      string // if set, stream returns this error
}

// FakeModel replays a scripted sequence of model responses.
type FakeModel struct {
	mu     sync.Mutex
	script []ScriptEntry
	calls  []domain.ModelRequest
	index  int
}

// NewFakeModel creates a FakeModel that replays the given script.
func NewFakeModel(script ...ScriptEntry) *FakeModel {
	return &FakeModel{script: script}
}

func (m *FakeModel) Stream(_ context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	m.calls = append(m.calls, req)

	if m.index >= len(m.script) {
		return nil, fmt.Errorf("fake model: script exhausted (index=%d, len=%d)", m.index, len(m.script))
	}
	entry := m.script[m.index]
	m.index++

	if entry.Error != "" {
		return nil, fmt.Errorf("%s", entry.Error)
	}

	return newFakeStream(entry), nil
}

// Calls returns all model requests received.
func (m *FakeModel) Calls() []domain.ModelRequest {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]domain.ModelRequest, len(m.calls))
	copy(out, m.calls)
	return out
}

type fakeStream struct {
	entry  ScriptEntry
	events []domain.ModelEvent
	pos    int
	closed bool
}

func newFakeStream(entry ScriptEntry) *fakeStream {
	var events []domain.ModelEvent

	if entry.Text != "" {
		events = append(
			events,
			domain.ModelEvent{Kind: domain.ModelEventTextStart},
			domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: entry.Text},
			domain.ModelEvent{Kind: domain.ModelEventTextEnd},
		)
	}

	for i, tc := range entry.ToolCalls {
		events = append(
			events,
			domain.ModelEvent{Kind: domain.ModelEventToolCallStart, ToolIndex: i, ToolID: tc.ID.String(), ToolName: tc.Name},
			domain.ModelEvent{Kind: domain.ModelEventToolArgsDelta, ToolIndex: i, ToolArgs: string(tc.Arguments)},
			domain.ModelEvent{Kind: domain.ModelEventToolCallEnd, ToolIndex: i},
		)
	}

	if entry.UsageIn > 0 || entry.UsageOut > 0 {
		events = append(events, domain.ModelEvent{
			Kind:         domain.ModelEventUsage,
			InputTokens:  entry.UsageIn,
			OutputTokens: entry.UsageOut,
		})
	}

	events = append(events, domain.ModelEvent{
		Kind:       domain.ModelEventResponseEnd,
		StopReason: entry.StopReason,
	})

	return &fakeStream{entry: entry, events: events}
}

func (s *fakeStream) Recv() (domain.ModelEvent, error) {
	if s.closed {
		return domain.ModelEvent{}, fmt.Errorf("stream closed")
	}
	if s.pos >= len(s.events) {
		return domain.ModelEvent{}, io.EOF
	}
	evt := s.events[s.pos]
	s.pos++
	return evt, nil
}

func (s *fakeStream) Close() error {
	s.closed = true
	return nil
}
