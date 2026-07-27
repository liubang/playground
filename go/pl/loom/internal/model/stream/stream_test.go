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

package stream

import (
	"context"
	"errors"
	"io"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestStreamDeliversEventsThenEOF(t *testing.T) {
	s := Start(context.Background(), io.NopCloser(strings.NewReader("")), func(ctx context.Context, body io.Reader, emit Emitter) {
		emit(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
		emit(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "hi"})
		emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})
	})
	defer s.Close()

	var kinds []domain.ModelEventKind
	for {
		evt, err := s.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			t.Fatalf("Recv: %v", err)
		}
		kinds = append(kinds, evt.Kind)
	}
	if len(kinds) != 3 {
		t.Fatalf("kinds = %v", kinds)
	}
	if kinds[0] != domain.ModelEventResponseStart || kinds[2] != domain.ModelEventResponseEnd {
		t.Fatalf("kinds = %v", kinds)
	}
}

func TestStreamCloseStopsEmitting(t *testing.T) {
	started := make(chan struct{})
	var emitResult atomic.Int32
	s := Start(context.Background(), io.NopCloser(strings.NewReader("")), func(ctx context.Context, body io.Reader, emit Emitter) {
		close(started)
		// Emit until the consumer goes away; each false return must stop
		// the pump promptly.
		for i := 0; ; i++ {
			if !emit(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "x"}) {
				emitResult.Add(1)
				return
			}
			if i > 1<<20 {
				t.Error("pump never observed stream closure")
				return
			}
		}
	})

	<-started
	// Give the pump a moment to fill the buffer, then close mid-flight.
	time.Sleep(10 * time.Millisecond)
	if err := s.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	// Drain; Recv must terminate with EOF after buffered events.
	for {
		if _, err := s.Recv(); err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			t.Fatalf("Recv: %v", err)
		}
	}
	if emitResult.Load() != 1 {
		t.Fatalf("pump did not stop after close (emitResult=%d)", emitResult.Load())
	}
}

func TestStreamCloseIsIdempotent(t *testing.T) {
	s := Start(context.Background(), io.NopCloser(strings.NewReader("")), func(ctx context.Context, body io.Reader, emit Emitter) {
		emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})
	})
	if err := s.Close(); err != nil {
		t.Fatalf("first Close: %v", err)
	}
	if err := s.Close(); err != nil {
		t.Fatalf("second Close: %v", err)
	}
}

func TestStreamPumpPanicBecomesStreamError(t *testing.T) {
	s := Start(context.Background(), io.NopCloser(strings.NewReader("")), func(ctx context.Context, body io.Reader, emit Emitter) {
		emit(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
		panic("protocol mapping bug")
	})
	defer s.Close()

	var kinds []domain.ModelEventKind
	var streamErr string
	for {
		evt, err := s.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			t.Fatalf("Recv: %v", err)
		}
		kinds = append(kinds, evt.Kind)
		if evt.Kind == domain.ModelEventStreamError {
			streamErr = evt.Error
		}
	}
	want := []domain.ModelEventKind{
		domain.ModelEventResponseStart,
		domain.ModelEventStreamError,
		domain.ModelEventResponseEnd,
	}
	if len(kinds) != len(want) {
		t.Fatalf("kinds = %v, want %v", kinds, want)
	}
	for i := range want {
		if kinds[i] != want[i] {
			t.Fatalf("kinds = %v, want %v", kinds, want)
		}
	}
	if !strings.Contains(streamErr, "protocol mapping bug") {
		t.Fatalf("stream error = %q", streamErr)
	}
}

func TestStreamContextCancelUnblocksPump(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	body := io.NopCloser(strings.NewReader(""))
	pumpDone := make(chan struct{})
	s := Start(ctx, body, func(pumpCtx context.Context, body io.Reader, emit Emitter) {
		defer close(pumpDone)
		<-pumpCtx.Done()
	})

	cancel()
	select {
	case <-pumpDone:
	case <-time.After(2 * time.Second):
		t.Fatal("pump did not observe cancellation")
	}
	_ = s.Close()
}
