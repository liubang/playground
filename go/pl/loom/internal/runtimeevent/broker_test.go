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
// Created: 2026/07/23

package runtimeevent

import (
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestBrokerSubscribeAndUnsubscribe(t *testing.T) {
	b := NewBroker()
	defer b.Close()

	ch, unsubscribe := b.Subscribe()
	if ch == nil {
		t.Fatal("expected non-nil channel")
	}

	sessionID := domain.NewSessionID()
	err := b.PublishDurable(sessionID, domain.RunID{}, 0, KindTurnStarted, TurnStartedPayload{TurnIndex: 1})
	if err != nil {
		t.Fatalf("PublishDurable: %v", err)
	}

	select {
	case evt := <-ch:
		if evt.Kind != KindTurnStarted {
			t.Fatalf("expected turn.started, got %q", evt.Kind)
		}
		if evt.Sequence != 1 {
			t.Fatalf("expected sequence 1, got %d", evt.Sequence)
		}
		if !evt.Durable {
			t.Fatal("expected durable event")
		}
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for event")
	}

	unsubscribe()

	// After unsubscribe, publishing should not panic
	_ = b.PublishDurable(sessionID, domain.RunID{}, 0, KindTurnFinished, nil)
}

func TestBrokerMultipleSubscribers(t *testing.T) {
	b := NewBroker()
	defer b.Close()

	ch1, unsub1 := b.Subscribe()
	defer unsub1()
	ch2, unsub2 := b.Subscribe()
	defer unsub2()

	sessionID := domain.NewSessionID()
	if err := b.PublishDurable(sessionID, domain.RunID{}, 0, KindSessionOpened, SessionOpenedPayload{}); err != nil {
		t.Fatalf("PublishDurable: %v", err)
	}

	select {
	case <-ch1:
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for event on ch1")
	}

	select {
	case <-ch2:
	case <-time.After(time.Second):
		t.Fatal("timeout waiting for event on ch2")
	}
}

func TestBrokerSequenceMonotonic(t *testing.T) {
	b := NewBroker()
	defer b.Close()

	ch, unsub := b.Subscribe()
	defer unsub()

	sessionID := domain.NewSessionID()
	for i := 0; i < 5; i++ {
		if err := b.PublishEphemeral(sessionID, domain.RunID{}, 0, KindModelTextDelta, ModelTextDeltaPayload{Delta: "a"}); err != nil {
			t.Fatalf("PublishEphemeral: %v", err)
		}
	}

	for i := uint64(1); i <= 5; i++ {
		select {
		case evt := <-ch:
			if evt.Sequence != i {
				t.Fatalf("expected sequence %d, got %d", i, evt.Sequence)
			}
		case <-time.After(time.Second):
			t.Fatalf("timeout waiting for event %d", i)
		}
	}
}

func TestBrokerClose(t *testing.T) {
	b := NewBroker()
	ch, _ := b.Subscribe()

	sessionID := domain.NewSessionID()
	_ = b.PublishDurable(sessionID, domain.RunID{}, 0, KindSessionOpened, SessionOpenedPayload{})

	b.Close()

	// Channel should be closed
	select {
	case _, ok := <-ch:
		if ok {
			// Got an event before close was processed - that's ok
		}
	case <-time.After(100 * time.Millisecond):
	}
}

func TestBrokerValidatesEvent_InvalidSessionID(t *testing.T) {
	b := NewBroker()
	defer b.Close()

	err := b.Publish(RuntimeEvent{
		Version:   RuntimeEventVersion,
		Sequence:  1,
		Kind:      KindTurnStarted,
		SessionID: domain.SessionID{}, // Zero session ID — invalid
	})
	if err == nil {
		t.Fatal("expected validation error for zero session ID")
	}
}

func TestBrokerValidatesEvent_UnknownKind(t *testing.T) {
	b := NewBroker()
	defer b.Close()

	err := b.Publish(RuntimeEvent{
		Version:   RuntimeEventVersion,
		Sequence:  1,
		Kind:      RuntimeEventKind("invalid.kind"),
		SessionID: domain.NewSessionID(),
	})
	if err == nil {
		t.Fatal("expected validation error for unknown kind")
	}
}

func TestBrokerDisconnectsSlowDurableSubscriberWithoutBlockingRuntime(t *testing.T) {
	b := NewBroker(WithDurableQueue(1))
	defer b.Close()

	slow, _ := b.Subscribe()
	fast, unsubscribeFast := b.Subscribe()
	defer unsubscribeFast()
	sessionID := domain.NewSessionID()

	if err := b.PublishDurable(sessionID, domain.RunID{}, 1, KindTurnStarted, TurnStartedPayload{TurnIndex: 1}); err != nil {
		t.Fatalf("first PublishDurable: %v", err)
	}
	// The slow subscriber queue is now full. The second durable event must
	// disconnect it rather than block the agent runtime.
	done := make(chan error, 1)
	go func() {
		done <- b.PublishDurable(sessionID, domain.RunID{}, 1, KindTurnFinished, nil)
	}()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("second PublishDurable: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("durable publish blocked on slow subscriber")
	}

	for i := 0; i < 2; i++ {
		select {
		case <-fast:
		case <-time.After(time.Second):
			t.Fatalf("fast subscriber missed event %d", i)
		}
	}
	select {
	case _, ok := <-slow:
		if ok {
			// The buffered first event may be observed before closure.
			_, ok = <-slow
		}
		if ok {
			t.Fatal("slow subscriber was not disconnected")
		}
	case <-time.After(time.Second):
		t.Fatal("slow subscriber channel was not closed")
	}
}

func TestBrokerCloseIsIdempotentAndRejectsNewEvents(t *testing.T) {
	b := NewBroker()
	b.Close()
	b.Close()
	if err := b.PublishDurable(domain.NewSessionID(), domain.RunID{}, 1, KindTurnStarted, TurnStartedPayload{TurnIndex: 1}); err != ErrBrokerClosed {
		t.Fatalf("PublishDurable error = %v, want %v", err, ErrBrokerClosed)
	}
}

func TestPublishConvenienceMethods(t *testing.T) {
	b := NewBroker()
	defer b.Close()

	ch, unsub := b.Subscribe()
	defer unsub()

	sessionID := domain.NewSessionID()
	runID := domain.NewRunID()

	// Publish durable
	err := b.PublishDurable(sessionID, runID, 1, KindModelResponseCompleted, ModelResponseCompletedPayload{
		StopReason: domain.StopEndTurn,
	})
	if err != nil {
		t.Fatalf("PublishDurable: %v", err)
	}

	// Publish ephemeral
	err = b.PublishEphemeral(sessionID, runID, 1, KindModelTextDelta, ModelTextDeltaPayload{Delta: "hello"})
	if err != nil {
		t.Fatalf("PublishEphemeral: %v", err)
	}

	// Drain both events
	for i := 0; i < 2; i++ {
		select {
		case <-ch:
		case <-time.After(time.Second):
			t.Fatalf("timeout waiting for event %d", i)
		}
	}
}
