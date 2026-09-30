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

// Package stream provides the canonical event-stream plumbing shared by
// model providers: a pump goroutine converts one provider response body
// into domain events delivered over a buffered channel, while Recv/Close
// give the consumer a pull-based, cancellation-safe interface.
package stream

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Emitter delivers one canonical event to the consumer. It returns false
// once the stream has been closed, at which point the pump must abort
// promptly — the consumer is gone and further events would block.
type Emitter func(domain.ModelEvent) bool

// Pump converts one provider response body into canonical events. It must
// return when the body is exhausted, when ctx is cancelled, or when emit
// returns false; it must not emit a terminal event after an error return
// path that the provider already finalized. Protocol-specific framing and
// mapping live entirely inside the pump.
type Pump func(ctx context.Context, body io.Reader, emit Emitter)

// Stream implements domain.ModelStream by driving a Pump in a goroutine.
type Stream struct {
	cancel    context.CancelFunc
	body      io.ReadCloser
	events    chan domain.ModelEvent
	closed    chan struct{}
	closeOnce sync.Once
}

// Options bounds one stream against a wedged peer. A streaming LLM
// response may legitimately run for many minutes, so a fixed per-request
// timeout cannot distinguish "slow but progressing" from "silent and
// wedged" — these two timers express exactly that distinction. Zero
// fields disable the corresponding watchdog.
type Options struct {
	// IdleTimeout bounds the silence between body reads: any received
	// byte resets the timer (SSE heartbeat comments count — they prove
	// the peer is alive). Firing means the connection stopped delivering
	// data mid-response, the exact failure mode of a gateway that
	// accepted the request and then hung.
	IdleTimeout time.Duration
	// MaxDuration bounds the stream's whole lifetime regardless of
	// activity: a peer trickling keepalive frames while its upstream is
	// dead never trips the idle watchdog, so an absolute cap is the
	// backstop for heartbeat-masked wedges.
	MaxDuration time.Duration
}

// Start launches pump in a goroutine and returns the readable stream.
// Cancelling ctx (or calling Close) aborts the body read; Close is
// idempotent and always releases the body. A panicking pump is converted
// into StreamError + ResponseEnd(ProviderError) instead of crashing the
// process: a protocol-mapping bug must not take down the agent session.
func Start(ctx context.Context, body io.ReadCloser, pump Pump) *Stream {
	return StartWithOptions(ctx, body, pump, Options{})
}

// StartWithOptions is Start with liveness bounds: when either watchdog
// fires, the body read fails with a sticky error (errStreamStalled /
// errStreamExpired) instead of blocking forever. The error surfaces
// through the pump's ordinary read-error path, so providers classify it
// as a transient, retryable stream failure — a wedge becomes a retry
// rather than a frozen run.
func StartWithOptions(ctx context.Context, body io.ReadCloser, pump Pump, opts Options) *Stream {
	if opts.IdleTimeout > 0 || opts.MaxDuration > 0 {
		body = newWatchdogBody(body, opts)
	}
	streamCtx, cancel := context.WithCancel(ctx)
	s := &Stream{
		cancel: cancel,
		body:   body,
		events: make(chan domain.ModelEvent, 64),
		closed: make(chan struct{}),
	}
	go func() {
		defer close(s.events)
		defer s.Close()
		defer func() {
			if r := recover(); r != nil {
				s.emit(domain.ModelEvent{
					Kind:  domain.ModelEventStreamError,
					Error: fmt.Sprintf("model stream panic: %v", r),
				})
				s.emit(domain.ModelEvent{
					Kind:       domain.ModelEventResponseEnd,
					StopReason: domain.StopProviderError,
				})
			}
		}()
		pump(streamCtx, body, s.emit)
	}()
	return s
}

// Recv returns the next canonical event, or io.EOF once the pump has
// finished and the event channel is drained.
func (s *Stream) Recv() (domain.ModelEvent, error) {
	evt, ok := <-s.events
	if !ok {
		return domain.ModelEvent{}, io.EOF
	}
	return evt, nil
}

// Close cancels the pump's context and releases the response body.
func (s *Stream) Close() error {
	var err error
	s.closeOnce.Do(func() {
		close(s.closed)
		s.cancel()
		err = s.body.Close()
	})
	return err
}

func (s *Stream) emit(evt domain.ModelEvent) bool {
	select {
	case <-s.closed:
		return false
	case s.events <- evt:
		return true
	}
}

// Watchdog failure sentinels. They stay unexported: providers route any
// non-EOF read error to their transient-failure path, which is exactly
// the classification a wedge deserves, and the wrapped message carries
// the diagnosable detail (which watchdog, which bound).
var (
	errStreamStalled = errors.New("model stream stalled")
	errStreamExpired = errors.New("model stream lifetime exceeded")
)

type readResult struct {
	n   int
	err error
}

// watchdogBody wraps a response body with the two Options timers. A body
// read cannot be interrupted from the outside, so each read runs in its
// own goroutine and Read selects on its result versus the timers; on a
// timeout the source is closed (releasing the goroutine, whose result
// the buffered channel absorbs) and the failure becomes sticky.
type watchdogBody struct {
	src      io.ReadCloser
	idleDur  time.Duration
	maxDur   time.Duration
	maxTimer *time.Timer

	mu        sync.Mutex
	failed    error
	closed    chan struct{}
	closeOnce sync.Once
}

func newWatchdogBody(src io.ReadCloser, opts Options) *watchdogBody {
	w := &watchdogBody{
		src:     src,
		idleDur: opts.IdleTimeout,
		maxDur:  opts.MaxDuration,
		closed:  make(chan struct{}),
	}
	if opts.MaxDuration > 0 {
		w.maxTimer = time.NewTimer(opts.MaxDuration)
	}
	return w
}

func (w *watchdogBody) Read(p []byte) (int, error) {
	if err := w.err(); err != nil {
		return 0, err
	}

	resCh := make(chan readResult, 1)
	go func() {
		n, err := w.src.Read(p)
		resCh <- readResult{n, err}
	}()

	var idleC <-chan time.Time
	var idleTimer *time.Timer
	if w.idleDur > 0 {
		idleTimer = time.NewTimer(w.idleDur)
		idleC = idleTimer.C
	}
	var maxC <-chan time.Time
	if w.maxTimer != nil {
		maxC = w.maxTimer.C
	}

	select {
	case r := <-resCh:
		if idleTimer != nil {
			idleTimer.Stop()
		}
		return r.n, r.err
	case <-idleC:
		w.shutdown(fmt.Errorf("%w: no data for %s", errStreamStalled, w.idleDur))
		return 0, w.err()
	case <-maxC:
		w.shutdown(fmt.Errorf("%w: capped at %s", errStreamExpired, w.maxDur))
		return 0, w.err()
	case <-w.closed:
		if idleTimer != nil {
			idleTimer.Stop()
		}
		// Close released the pending read; a watchdog-induced failure
		// takes precedence over the read's own (closed-body) error.
		r := <-resCh
		if err := w.err(); err != nil {
			return 0, err
		}
		return r.n, r.err
	}
}

func (w *watchdogBody) err() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.failed
}

// shutdown records the sticky failure and closes the source, unblocking
// the in-flight read.
func (w *watchdogBody) shutdown(err error) {
	w.closeOnce.Do(func() {
		w.mu.Lock()
		w.failed = err
		w.mu.Unlock()
		close(w.closed)
		_ = w.src.Close()
	})
}

func (w *watchdogBody) Close() error {
	var err error
	w.closeOnce.Do(func() {
		close(w.closed)
		if w.maxTimer != nil {
			w.maxTimer.Stop()
		}
		err = w.src.Close()
	})
	return err
}
