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
//
// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/23

package runtimeevent

import (
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Observer receives runtime events. Implementations must not block on
// ephemeral events; durable events may block briefly.
type Observer interface {
	// ObserveEphemeral receives non-durable events (text deltas, progress).
	// Must be non-blocking; events may be coalesced or dropped.
	ObserveEphemeral(RuntimeEvent)
	// ObserveDurable receives durable events.
	ObserveDurable(RuntimeEvent) error
}

// ErrBrokerClosed indicates that an event cannot be published because the
// broker has already stopped accepting events.
var ErrBrokerClosed = fmt.Errorf("runtime event broker is closed")

// Broker distributes runtime events to subscribers with sequence numbering.
// It is intentionally a non-blocking fan-out boundary: durable runtime events
// are already backed by the domain event store, so a disconnected or slow UI
// must never stall the agent, tool pipes, or persistence path.
type Broker struct {
	mu           sync.Mutex
	sequence     uint64
	subscribers  map[uint64]*subscriber
	nextID       uint64
	durableQueue int
	closed       bool
}

type subscriber struct {
	id uint64
	ch chan RuntimeEvent
}

// BrokerOption configures a Broker.
type BrokerOption func(*Broker)

// WithDurableQueue sets the event queue capacity per subscriber.
func WithDurableQueue(capacity int) BrokerOption {
	return func(b *Broker) {
		if capacity > 0 {
			b.durableQueue = capacity
		}
	}
}

// NewBroker creates a new Broker.
func NewBroker(opts ...BrokerOption) *Broker {
	b := &Broker{
		subscribers:  make(map[uint64]*subscriber),
		durableQueue: 256,
	}
	for _, opt := range opts {
		opt(b)
	}
	return b
}

// Subscribe registers a new subscriber and returns an unsubscribe function.
// The returned channel receives future runtime events until it is unsubscribed
// or the broker is closed.
func (b *Broker) Subscribe() (<-chan RuntimeEvent, func()) {
	b.mu.Lock()
	if b.closed {
		ch := make(chan RuntimeEvent)
		close(ch)
		b.mu.Unlock()
		return ch, func() {}
	}
	id := b.nextID
	b.nextID++
	ch := make(chan RuntimeEvent, b.durableQueue)
	b.subscribers[id] = &subscriber{id: id, ch: ch}
	b.mu.Unlock()

	var once sync.Once
	unsubscribe := func() {
		once.Do(func() {
			b.removeSubscriber(id)
		})
	}
	return ch, unsubscribe
}

func (b *Broker) removeSubscriber(id uint64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	sub, ok := b.subscribers[id]
	if !ok {
		return
	}
	delete(b.subscribers, id)
	close(sub.ch)
}

// Publish assigns a sequence and delivers an event without blocking. Ephemeral
// events are dropped for a full subscriber queue. A subscriber that cannot keep
// up with durable events is disconnected: its canonical history remains in the
// event store and it must recover through a snapshot/replay rather than block
// the runtime.
func (b *Broker) Publish(evt RuntimeEvent) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return ErrBrokerClosed
	}

	b.sequence++
	evt.Sequence = b.sequence
	evt.Time = time.Now().UTC()
	evt.Version = RuntimeEventVersion
	if err := evt.Validate(); err != nil {
		return err
	}

	for id, sub := range b.subscribers {
		select {
		case sub.ch <- evt:
		default:
			if evt.Durable {
				// Do not allow a dead frontend to block the runtime. Because this
				// happens under b.mu, no concurrent publisher can send after close.
				delete(b.subscribers, id)
				close(sub.ch)
			}
		}
	}
	return nil
}

// PublishDurable publishes a durable event.
func (b *Broker) PublishDurable(sessionID domain.SessionID, runID domain.RunID, turn int, kind RuntimeEventKind, payload any) error {
	return b.publish(true, sessionID, runID, turn, kind, payload)
}

// PublishEphemeral publishes an ephemeral event.
func (b *Broker) PublishEphemeral(sessionID domain.SessionID, runID domain.RunID, turn int, kind RuntimeEventKind, payload any) error {
	return b.publish(false, sessionID, runID, turn, kind, payload)
}

func (b *Broker) publish(durable bool, sessionID domain.SessionID, runID domain.RunID, turn int, kind RuntimeEventKind, payload any) error {
	var rawPayload json.RawMessage
	if payload != nil {
		data, err := json.Marshal(payload)
		if err != nil {
			return err
		}
		rawPayload = json.RawMessage(data)
	}
	return b.Publish(RuntimeEvent{
		SessionID: sessionID,
		RunID:     runID,
		Turn:      turn,
		Kind:      kind,
		Durable:   durable,
		Payload:   rawPayload,
	})
}

// Sequence returns the current sequence number.
func (b *Broker) Sequence() uint64 {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.sequence
}

// Close stops event delivery and closes every subscriber channel. It is
// idempotent and safe to race with Publish and unsubscribe.
func (b *Broker) Close() {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return
	}
	b.closed = true
	for id, sub := range b.subscribers {
		delete(b.subscribers, id)
		close(sub.ch)
	}
}
