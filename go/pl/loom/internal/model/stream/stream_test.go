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

// drainEvents collects events until EOF, returning the first StreamError
// message seen.
func drainEvents(t *testing.T, s *Stream) string {
	t.Helper()
	deadline := time.After(5 * time.Second)
	var streamErr string
	for {
		select {
		case <-deadline:
			t.Fatal("stream did not terminate")
		default:
		}
		evt, err := s.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return streamErr
			}
			t.Fatalf("Recv: %v", err)
		}
		if evt.Kind == domain.ModelEventStreamError {
			streamErr = evt.Error
		}
	}
}

// readFailPump reads the body until it fails, then surfaces the failure
// the way provider pumps do (StreamError + ResponseEnd).
func readFailPump(ctx context.Context, body io.Reader, emit Emitter) {
	buf := make([]byte, 4096)
	for {
		if _, err := body.Read(buf); err != nil {
			emit(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: err.Error(), Retryable: true})
			emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopProviderError})
			return
		}
	}
}

// TestWatchdogIdleTimeoutFires covers the wedged-gateway failure mode:
// the peer accepted the response and then went silent forever.
func TestWatchdogIdleTimeoutFires(t *testing.T) {
	pr, pw := io.Pipe()
	defer func() { _ = pw.Close() }()

	s := StartWithOptions(context.Background(), pr, readFailPump, Options{IdleTimeout: 50 * time.Millisecond})
	defer func() { _ = s.Close() }()

	if got := drainEvents(t, s); !strings.Contains(got, "stalled") {
		t.Fatalf("stream error = %q, want stall mention", got)
	}
}

// TestWatchdogIdleResetByProgress proves the watchdog never fires while
// bytes keep arriving slower than the timeout — the "slow but alive"
// generation must not be clipped.
func TestWatchdogIdleResetByProgress(t *testing.T) {
	pr, pw := io.Pipe()
	go func() {
		defer func() { _ = pw.Close() }()
		for i := 0; i < 10; i++ {
			time.Sleep(30 * time.Millisecond)
			if _, err := pw.Write([]byte("x")); err != nil {
				return
			}
		}
	}()

	var delivered int
	s := StartWithOptions(context.Background(), pr, func(ctx context.Context, body io.Reader, emit Emitter) {
		buf := make([]byte, 4096)
		for {
			if _, err := body.Read(buf); err != nil {
				if errors.Is(err, io.EOF) {
					emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})
				} else {
					emit(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: err.Error()})
					emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopProviderError})
				}
				return
			}
			delivered++
		}
	}, Options{IdleTimeout: 150 * time.Millisecond})
	defer func() { _ = s.Close() }()

	if got := drainEvents(t, s); got != "" {
		t.Fatalf("unexpected stream error = %q", got)
	}
	if delivered == 0 {
		t.Fatal("no bytes delivered")
	}
}

// TestWatchdogMaxDurationFires covers the heartbeat-masked wedge: bytes
// keep trickling (the idle timer keeps resetting) but the stream never
// completes, so the absolute cap must end it.
func TestWatchdogMaxDurationFires(t *testing.T) {
	pr, pw := io.Pipe()
	go func() {
		defer func() { _ = pw.Close() }()
		for {
			if _, err := pw.Write([]byte(":")); err != nil {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
	}()

	s := StartWithOptions(context.Background(), pr, readFailPump, Options{
		IdleTimeout: time.Minute,
		MaxDuration: 80 * time.Millisecond,
	})
	defer func() { _ = s.Close() }()

	if got := drainEvents(t, s); !strings.Contains(got, "lifetime exceeded") {
		t.Fatalf("stream error = %q, want lifetime mention", got)
	}
}

// TestWatchdogStickyFailure asserts the failure persists across reads:
// after the watchdog fires, subsequent reads report it instead of
// re-blocking on the dead source.
func TestWatchdogStickyFailure(t *testing.T) {
	pr, pw := io.Pipe()
	defer func() { _ = pw.Close() }()

	body := newWatchdogBody(pr, Options{IdleTimeout: 50 * time.Millisecond})
	defer func() { _ = body.Close() }()

	buf := make([]byte, 16)
	_, err := body.Read(buf)
	if err == nil || !strings.Contains(err.Error(), "stalled") {
		t.Fatalf("first read err = %v", err)
	}
	_, err = body.Read(buf)
	if err == nil || !strings.Contains(err.Error(), "stalled") {
		t.Fatalf("second read err = %v (must be sticky)", err)
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
