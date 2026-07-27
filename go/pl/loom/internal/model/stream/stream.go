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
	"fmt"
	"io"
	"sync"

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

// Start launches pump in a goroutine and returns the readable stream.
// Cancelling ctx (or calling Close) aborts the body read; Close is
// idempotent and always releases the body. A panicking pump is converted
// into StreamError + ResponseEnd(ProviderError) instead of crashing the
// process: a protocol-mapping bug must not take down the agent session.
func Start(ctx context.Context, body io.ReadCloser, pump Pump) *Stream {
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
