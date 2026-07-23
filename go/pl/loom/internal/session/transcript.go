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

package session

import (
	"encoding/json"
	"fmt"
	"sort"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Transcript is the read-only canonical transcript projection for a session.
type Transcript struct {
	SessionID         domain.SessionID `json:"session_id"`
	Messages          []domain.Message `json:"messages"`
	LastEventSequence int64            `json:"last_event_sequence"`
}

// TranscriptPage is a deterministic page view over a canonical transcript.
type TranscriptPage struct {
	SessionID     domain.SessionID `json:"session_id"`
	AfterSequence int64            `json:"after_sequence,omitempty"`
	NextAfter     int64            `json:"next_after,omitempty"`
	HasMore       bool             `json:"has_more"`
	Messages      []domain.Message `json:"messages"`
}

// Replay rebuilds a canonical transcript from session events.
func Replay(events []domain.Event) (Transcript, error) {
	projector := newProjector()
	if err := projector.applyEvents(events, 0); err != nil {
		return Transcript{}, err
	}
	return projector.transcript(), nil
}

// ReplayFromCheckpoint rebuilds a transcript from a checkpoint plus later events.
func ReplayFromCheckpoint(ckpt domain.Checkpoint, events []domain.Event) (Transcript, error) {
	projector := newProjector()
	if ckpt.SessionID.IsZero() {
		return Transcript{}, fmt.Errorf("checkpoint session ID required")
	}
	projector.sessionID = ckpt.SessionID
	projector.lastEventSequence = ckpt.Sequence
	for _, msg := range sortedMessages(ckpt.Messages) {
		normalized, err := projector.normalizeMessage(msg)
		if err != nil {
			return Transcript{}, fmt.Errorf("checkpoint message %s: %w", msg.ID, err)
		}
		if err := projector.applyMessage(normalized); err != nil {
			return Transcript{}, fmt.Errorf("checkpoint message %s: %w", msg.ID, err)
		}
	}
	if err := projector.applyEvents(events, ckpt.Sequence); err != nil {
		return Transcript{}, err
	}
	return projector.transcript(), nil
}

// Validate checks the transcript ordering and message invariants.
func (t Transcript) Validate() error {
	if t.SessionID.IsZero() {
		return fmt.Errorf("session ID required")
	}
	lastSequence := int64(0)
	seenMessageIDs := map[string]struct{}{}
	for i, msg := range t.Messages {
		if err := msg.Validate(); err != nil {
			return fmt.Errorf("messages[%d]: %w", i, err)
		}
		if msg.Sequence <= 0 {
			return fmt.Errorf("messages[%d]: sequence must be positive", i)
		}
		if i > 0 && msg.Sequence <= lastSequence {
			return fmt.Errorf("messages[%d]: sequence must be strictly increasing", i)
		}
		if _, ok := seenMessageIDs[msg.ID.String()]; ok {
			return fmt.Errorf("messages[%d]: duplicate message ID %s", i, msg.ID)
		}
		seenMessageIDs[msg.ID.String()] = struct{}{}
		lastSequence = msg.Sequence
	}
	if t.LastEventSequence < 0 {
		return fmt.Errorf("last event sequence must be non-negative")
	}
	return nil
}

// CanonicalJSON renders the transcript using deterministic JSON.
func (t Transcript) CanonicalJSON() ([]byte, error) {
	if err := t.Validate(); err != nil {
		return nil, err
	}
	return json.Marshal(t)
}

// Page paginates the transcript by canonical message sequence.
func (t Transcript) Page(afterSequence int64, limit int) (TranscriptPage, error) {
	if err := t.Validate(); err != nil {
		return TranscriptPage{}, err
	}
	if afterSequence < 0 {
		return TranscriptPage{}, fmt.Errorf("after sequence must be non-negative")
	}
	if limit < 0 {
		return TranscriptPage{}, fmt.Errorf("limit must be non-negative")
	}

	start := 0
	for start < len(t.Messages) && t.Messages[start].Sequence <= afterSequence {
		start++
	}
	end := len(t.Messages)
	if limit > 0 && start+limit < end {
		end = start + limit
	}
	pageMessages := append([]domain.Message(nil), t.Messages[start:end]...)
	page := TranscriptPage{
		SessionID:     t.SessionID,
		AfterSequence: afterSequence,
		HasMore:       end < len(t.Messages),
		Messages:      pageMessages,
	}
	if len(pageMessages) > 0 {
		page.NextAfter = pageMessages[len(pageMessages)-1].Sequence
	} else {
		page.NextAfter = afterSequence
	}
	return page, nil
}

// CanonicalJSON renders the page using deterministic JSON.
func (p TranscriptPage) CanonicalJSON() ([]byte, error) {
	if p.SessionID.IsZero() {
		return nil, fmt.Errorf("session ID required")
	}
	return json.Marshal(p)
}

type projector struct {
	sessionID         domain.SessionID
	messages          []domain.Message
	messageByID       map[string]int
	messageBySequence map[int64]int
	lastEventSequence int64
	nextSequence      int64
}

func newProjector() *projector {
	return &projector{
		messageByID:       map[string]int{},
		messageBySequence: map[int64]int{},
		nextSequence:      1,
	}
}

func (p *projector) transcript() Transcript {
	out := Transcript{
		SessionID:         p.sessionID,
		Messages:          append([]domain.Message(nil), p.messages...),
		LastEventSequence: p.lastEventSequence,
	}
	return out
}

func (p *projector) applyEvents(events []domain.Event, minSequence int64) error {
	sorted := append([]domain.Event(nil), events...)
	sort.Slice(sorted, func(i, j int) bool {
		if sorted[i].Sequence == sorted[j].Sequence {
			return sorted[i].ID.String() < sorted[j].ID.String()
		}
		return sorted[i].Sequence < sorted[j].Sequence
	})

	seenEventSequences := map[int64]struct{}{}
	for _, evt := range sorted {
		if err := evt.Validate(); err != nil {
			return fmt.Errorf("event %s: %w", evt.ID, err)
		}
		if evt.Sequence <= minSequence {
			continue
		}
		if _, ok := seenEventSequences[evt.Sequence]; ok {
			return fmt.Errorf("duplicate event sequence %d", evt.Sequence)
		}
		seenEventSequences[evt.Sequence] = struct{}{}
		if p.lastEventSequence > 0 && evt.Sequence <= p.lastEventSequence {
			return fmt.Errorf("event sequence %d not increasing", evt.Sequence)
		}
		if p.sessionID.IsZero() {
			p.sessionID = evt.SessionID
		} else if evt.SessionID != p.sessionID {
			return fmt.Errorf("mixed session IDs: %s vs %s", p.sessionID, evt.SessionID)
		}
		if err := p.applyEvent(evt); err != nil {
			return fmt.Errorf("event %s (%s): %w", evt.ID, evt.Type, err)
		}
		p.lastEventSequence = evt.Sequence
	}
	if p.sessionID.IsZero() {
		return fmt.Errorf("session ID required")
	}
	return nil
}

func (p *projector) applyEvent(evt domain.Event) error {
	switch evt.Type {
	case domain.EventSessionCreated,
		domain.EventRunCreated,
		domain.EventRunStateChanged,
		domain.EventModelRequestStarted,
		domain.EventModelRequestFailed,
		domain.EventToolCallPrepared,
		domain.EventPermissionRequested,
		domain.EventPermissionResolved,
		domain.EventToolExecutionStarted,
		domain.EventToolExecutionCompleted,
		domain.EventFileChanged,
		domain.EventPlanRevised,
		domain.EventContextCompacted,
		domain.EventCheckpointCreated,
		domain.EventBudgetUpdated,
		domain.EventRunCompleted,
		domain.EventRunFailed,
		domain.EventRunCancelled:
		return nil
	case domain.EventUserMessageAdded, domain.EventModelResponseCompleted, domain.EventToolResultAdded:
		payload, err := domain.UnmarshalMessageEventPayload(evt.Payload)
		if err != nil {
			return err
		}
		msg := payload.Message
		if msg.Sequence == 0 {
			msg.Sequence = p.nextLogicalSequence(msg.ID)
		}
		normalized, err := p.normalizeMessage(msg)
		if err != nil {
			return err
		}
		return p.applyMessage(normalized)
	default:
		return fmt.Errorf("unsupported event type %q", evt.Type)
	}
}

func (p *projector) normalizeMessage(msg domain.Message) (domain.Message, error) {
	if msg.Status == "" {
		msg.Status = domain.MessageStatusFinal
	}
	if existingIndex, ok := p.messageByID[msg.ID.String()]; ok {
		existing := p.messages[existingIndex]
		if msg.Sequence == 0 {
			msg.Sequence = existing.Sequence
		}
		if msg.Sequence != existing.Sequence {
			return domain.Message{}, fmt.Errorf("message sequence changed from %d to %d", existing.Sequence, msg.Sequence)
		}
		if msg.Role != existing.Role {
			return domain.Message{}, fmt.Errorf("message role changed from %s to %s", existing.Role, msg.Role)
		}
		if msg.Revision == 0 {
			msg.Revision = existing.Revision + 1
		}
		if msg.Revision <= existing.Revision {
			return domain.Message{}, fmt.Errorf("message revision must increase")
		}
	} else {
		if msg.Sequence <= 0 {
			return domain.Message{}, fmt.Errorf("message sequence must be positive")
		}
		if msg.Revision == 0 {
			msg.Revision = 1
		}
	}

	parts, err := normalizeParts(msg.Parts)
	if err != nil {
		return domain.Message{}, err
	}
	msg.Parts = parts
	if err := msg.Validate(); err != nil {
		return domain.Message{}, err
	}
	return msg, nil
}

func (p *projector) applyMessage(msg domain.Message) error {
	if existingIndex, ok := p.messageByID[msg.ID.String()]; ok {
		existing := p.messages[existingIndex]
		if msg.Sequence != existing.Sequence {
			return fmt.Errorf("message sequence changed from %d to %d", existing.Sequence, msg.Sequence)
		}
		p.messages[existingIndex] = msg
		p.rebuildIndexes()
		return nil
	}
	if otherIndex, ok := p.messageBySequence[msg.Sequence]; ok {
		return fmt.Errorf("sequence %d already assigned to message %s", msg.Sequence, p.messages[otherIndex].ID)
	}
	p.messages = append(p.messages, msg)
	p.rebuildIndexes()
	if msg.Sequence >= p.nextSequence {
		p.nextSequence = msg.Sequence + 1
	}
	return nil
}

func (p *projector) rebuildIndexes() {
	sort.Slice(p.messages, func(i, j int) bool {
		return p.messages[i].Sequence < p.messages[j].Sequence
	})
	p.messageByID = make(map[string]int, len(p.messages))
	p.messageBySequence = make(map[int64]int, len(p.messages))
	for i, msg := range p.messages {
		p.messageByID[msg.ID.String()] = i
		p.messageBySequence[msg.Sequence] = i
	}
}

func (p *projector) nextLogicalSequence(messageID domain.MessageID) int64 {
	if existingIndex, ok := p.messageByID[messageID.String()]; ok {
		return p.messages[existingIndex].Sequence
	}
	seq := p.nextSequence
	p.nextSequence++
	return seq
}

func normalizeParts(parts []domain.ContentPart) ([]domain.ContentPart, error) {
	normalized := append([]domain.ContentPart(nil), parts...)
	allImplicit := len(normalized) > 0
	for i, part := range normalized {
		if i == 0 && part.PartIndex == 0 {
			continue
		}
		if part.PartIndex != 0 {
			allImplicit = false
			break
		}
	}
	if allImplicit {
		for i := range normalized {
			normalized[i].PartIndex = i
		}
	}
	seen := map[int]struct{}{}
	for i := range normalized {
		if normalized[i].PartIndex < 0 {
			return nil, fmt.Errorf("part[%d]: part_index must be non-negative", i)
		}
		if _, ok := seen[normalized[i].PartIndex]; ok {
			return nil, fmt.Errorf("part[%d]: duplicate part_index %d", i, normalized[i].PartIndex)
		}
		seen[normalized[i].PartIndex] = struct{}{}
		if err := normalized[i].Validate(); err != nil {
			return nil, fmt.Errorf("part[%d]: %w", i, err)
		}
	}
	return normalized, nil
}

func sortedMessages(messages []domain.Message) []domain.Message {
	out := append([]domain.Message(nil), messages...)
	sort.Slice(out, func(i, j int) bool {
		if out[i].Sequence == out[j].Sequence {
			if out[i].Revision == out[j].Revision {
				return out[i].ID.String() < out[j].ID.String()
			}
			return out[i].Revision < out[j].Revision
		}
		return out[i].Sequence < out[j].Sequence
	})
	return out
}
