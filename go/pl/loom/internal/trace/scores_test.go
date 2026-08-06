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
// Created: 2026/08/06

package trace

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"
)

// scoreSink captures score API requests for assertions.
type scoreSink struct {
	mu   sync.Mutex
	got  []scoreRequest
	auth string
}

func (s *scoreSink) handler(t *testing.T) http.Handler {
	t.Helper()
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/public/scores" {
			t.Errorf("unexpected path %q", r.URL.Path)
		}
		var req scoreRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Errorf("decode score request: %v", err)
		}
		s.mu.Lock()
		s.got = append(s.got, req)
		s.auth = r.Header.Get("Authorization")
		s.mu.Unlock()
		w.WriteHeader(http.StatusOK)
	})
}

func (s *scoreSink) last(t *testing.T) scoreRequest {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.got) == 0 {
		t.Fatal("no score request received")
	}
	return s.got[len(s.got)-1]
}

func TestScoreClientFeedbackPayload(t *testing.T) {
	sink := &scoreSink{}
	ts := httptest.NewServer(sink.handler(t))
	defer ts.Close()

	c := newScoreClient(ts.URL, "pk", "sk", "test", nil)
	c.submitFeedback("trace-1", "user_feedback", 1, "great answer")
	c.waitIdle(2 * time.Second)

	got := sink.last(t)
	if got.TraceID != "trace-1" || got.Name != "user_feedback" {
		t.Fatalf("trace/name = %q/%q, want trace-1/user_feedback", got.TraceID, got.Name)
	}
	if got.Value != 1 || got.DataType != scoreDataTypeBoolean {
		t.Fatalf("value/dataType = %v/%q, want 1/BOOLEAN", got.Value, got.DataType)
	}
	if got.Comment != "great answer" || got.Environment != "test" {
		t.Fatalf("comment/env = %q/%q", got.Comment, got.Environment)
	}
	if got.ID == "" {
		t.Fatal("feedback score must carry a deterministic id for idempotent re-votes")
	}
	if sink.auth != basicAuthHeader("pk", "sk") {
		t.Fatalf("authorization header mismatch: %q", sink.auth)
	}

	// Same (trace, name) must derive the same id: re-votes overwrite in place.
	first := got.ID
	c.submitFeedback("trace-1", "user_feedback", 0, "")
	c.waitIdle(2 * time.Second)
	if second := sink.last(t); second.ID != first {
		t.Fatalf("re-vote id = %q, want stable %q", second.ID, first)
	} else if second.Value != 0 || second.Comment != "" {
		t.Fatalf("re-vote value/comment = %v/%q, want 0/empty", second.Value, second.Comment)
	}

	// A different trace derives a different id.
	c.submitFeedback("trace-2", "user_feedback", 1, "")
	c.waitIdle(2 * time.Second)
	if third := sink.last(t); third.ID == first {
		t.Fatal("distinct traces must not share a feedback score id")
	}
}

func TestOTelRecorderScoreTrace(t *testing.T) {
	sink := &scoreSink{}
	ts := httptest.NewServer(sink.handler(t))
	defer ts.Close()

	rec := &otelRecorder{scores: newScoreClient(ts.URL, "pk", "sk", "test", nil)}
	if !rec.ScoreTrace(context.Background(), "trace-9", "user_feedback", 1, "") {
		t.Fatal("ScoreTrace delivered = false, want true with a configured score client")
	}
	rec.scores.waitIdle(2 * time.Second)
	if got := sink.last(t); got.TraceID != "trace-9" || got.DataType != scoreDataTypeBoolean {
		t.Fatalf("delivered score = %+v", got)
	}

	// No score client (tracing off) or empty trace id: not delivered.
	empty := &otelRecorder{}
	if empty.ScoreTrace(context.Background(), "trace-9", "user_feedback", 1, "") {
		t.Fatal("ScoreTrace without score client must report not-delivered")
	}
	if rec.ScoreTrace(context.Background(), "", "user_feedback", 1, "") {
		t.Fatal("ScoreTrace with empty trace id must report not-delivered")
	}
}

func TestNoopRecorderFeedbackSurface(t *testing.T) {
	if Noop().ScoreTrace(context.Background(), "trace-1", "user_feedback", 1, "") {
		t.Fatal("noop recorder must report not-delivered so callers can surface tracing_disabled")
	}
	_, run := Noop().StartRun(context.Background(), RunMeta{})
	if run.TraceID() != "" {
		t.Fatalf("noop run TraceID = %q, want empty", run.TraceID())
	}
}
