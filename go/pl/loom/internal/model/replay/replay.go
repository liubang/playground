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

package replay

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ReplayModel serves recorded model calls back without any provider I/O
// (REPLAY_TESTING_DESIGN §4). Calls are matched PURELY POSITIONALLY per
// binding key: replay inputs are deterministic (scripted prompts,
// recorded responses, seeded workspace), so the Kth call of a session
// must be the recorded Kth call — any drift fails loud via "script
// exhausted", "unrecorded session", or the teardown AssertConsumed
// check, and content drift surfaces via the fingerprint warning.
type ReplayModel struct {
	opts    options
	scripts map[string]*Script

	mu       sync.Mutex
	cursors  map[string]int
	bound    map[string]bool
	hangViol []string
}

// NewReplayModel loads every recording under the scenario directory.
func NewReplayModel(dir string, opts ...Option) (*ReplayModel, error) {
	o := resolveOptions(opts)
	scripts, err := LoadScripts(dir, o.scrubPaths)
	if err != nil {
		return nil, err
	}
	return &ReplayModel{
		opts:    o,
		scripts: scripts,
		cursors: make(map[string]int),
		bound:   make(map[string]bool),
	}, nil
}

func (m *ReplayModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	key := BindingKeyFrom(ctx)
	m.mu.Lock()
	script, ok := m.scripts[key]
	if !ok {
		m.mu.Unlock()
		return nil, fmt.Errorf("replay: a model call arrived from an unrecorded session (binding key %q); "+
			"the scenario recorded %d session(s) — re-record it", key, len(m.scripts))
	}
	m.bound[key] = true
	idx := m.cursors[key]
	m.cursors[key] = idx + 1
	m.mu.Unlock()

	if idx >= len(script.Calls) {
		return nil, fmt.Errorf("replay: script exhausted — session %q requested model call #%d "+
			"but its recording has only %d; re-record the scenario", key, idx+1, len(script.Calls))
	}
	call := script.Calls[idx]

	if call.Fingerprint != "" {
		if live := Fingerprint(req, m.opts.scrubPaths); live != call.Fingerprint {
			msg := fmt.Sprintf("replay: request fingerprint drifted on session %q call #%d "+
				"(recorded %s, live %s): the request content changed since recording — "+
				"legitimate prompt edits only need a golden refresh, anything else needs a re-record",
				key, idx+1, call.Fingerprint, live)
			if detail := driftDetail(call, req, m.opts.scrubPaths); detail != "" {
				msg += "\n" + detail
			}
			if m.opts.strict {
				return nil, errors.New(msg)
			}
			m.opts.warnf("%s", msg)
		}
	}

	return &replayStream{
		ctx:     ctx,
		events:  call.Events,
		outcome: call.Outcome,
		errText: call.ErrText,
		onHangSettled: func(cancelled bool) {
			if cancelled {
				return
			}
			m.mu.Lock()
			defer m.mu.Unlock()
			m.hangViol = append(m.hangViol, fmt.Sprintf(
				"session %q call #%d: hang entry was closed without cancellation — "+
					"the cancel path under test silently changed", key, idx+1,
			))
		},
	}, nil
}

// AssertConsumed is the teardown check (§4.2): every recorded session
// must have been exercised and every recorded call consumed, and every
// hang entry must have ended by cancellation. Anything less means the
// agent under test drove fewer calls than the recording — an early exit
// or a different path that would otherwise pass silently.
func (m *ReplayModel) AssertConsumed() error {
	m.mu.Lock()
	defer m.mu.Unlock()
	var problems []string
	for key, script := range m.scripts {
		if !m.bound[key] {
			problems = append(problems, fmt.Sprintf("recorded session %q never made a model call", key))
			continue
		}
		if consumed := m.cursors[key]; consumed < len(script.Calls) {
			problems = append(problems, fmt.Sprintf("session %q consumed %d/%d recorded call(s)", key, consumed, len(script.Calls)))
		}
	}
	problems = append(problems, m.hangViol...)
	if len(problems) > 0 {
		return fmt.Errorf("replay: fixture not fully consumed — %s", strings.Join(problems, "; "))
	}
	return nil
}

// replayStream replays one recorded call. The events replay verbatim;
// the terminator reproduces the recorded outcome.
type replayStream struct {
	ctx     context.Context
	events  []domain.ModelEvent
	outcome CallOutcome
	errText string

	pos           int
	hung          bool // the hang position was reached
	settled       bool // terminal outcome was delivered
	onHangSettled func(cancelled bool)
}

func (s *replayStream) Recv() (domain.ModelEvent, error) {
	if s.pos < len(s.events) {
		evt := s.events[s.pos]
		s.pos++
		return evt, nil
	}
	if s.outcome == OutcomeHang {
		s.hung = true
		// Block like the live stream did; the consumer's cancellation is
		// the only way out.
		<-s.ctx.Done()
		s.settled = true
		return domain.ModelEvent{}, s.ctx.Err()
	}
	s.settled = true
	if s.outcome == OutcomeError {
		return domain.ModelEvent{}, errors.New(s.errText)
	}
	return domain.ModelEvent{}, io.EOF
}

func (s *replayStream) Close() error {
	if s.hung && !s.settled && s.onHangSettled != nil {
		s.onHangSettled(s.ctx.Err() != nil)
	}
	return nil
}
