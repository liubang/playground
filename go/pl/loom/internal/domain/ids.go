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

package domain

import (
	"crypto/rand"
	"encoding/json"
	"fmt"
	"io"
	"strings"
)

// ID represents a strongly-typed identifier with a human-readable prefix.
type ID[T ~string] struct{ val T }

func newID[T ~string](prefix string) ID[T] {
	var buf [16]byte
	_, _ = io.ReadFull(rand.Reader, buf[:])
	return ID[T]{val: T(fmt.Sprintf("%s_%x", prefix, buf[:]))}
}

func (id ID[T]) String() string { return string(id.val) }

func (id ID[T]) IsZero() bool { return id.val == "" }

// MarshalJSON implements json.Marshaler.
func (id ID[T]) MarshalJSON() ([]byte, error) {
	return json.Marshal(string(id.val))
}

// UnmarshalJSON implements json.Unmarshaler.
func (r *ID[T]) UnmarshalJSON(data []byte) error {
	var s string
	if err := json.Unmarshal(data, &s); err != nil {
		return err
	}
	r.val = T(s)
	return nil
}

// ParseID parses a string into a strongly-typed ID.
// Returns an error if the string is empty.
func ParseID[T ~string](s string) (ID[T], error) {
	if s == "" {
		return ID[T]{}, fmt.Errorf("empty ID")
	}
	return ID[T]{val: T(s)}, nil
}

// --- concrete ID types ---

type (
	sessionIDT    string //nolint:unused // used via SessionID below
	runIDT        string //nolint:unused
	turnIDT       string //nolint:unused
	messageIDT    string //nolint:unused
	toolCallIDT   string //nolint:unused
	eventIDT      string //nolint:unused
	artifactIDT   string //nolint:unused
	checkpointIDT string //nolint:unused
)

type (
	SessionID    = ID[sessionIDT]
	RunID        = ID[runIDT]
	TurnID       = ID[turnIDT]
	MessageID    = ID[messageIDT]
	ToolCallID   = ID[toolCallIDT]
	EventID      = ID[eventIDT]
	ArtifactID   = ID[artifactIDT]
	CheckpointID = ID[checkpointIDT]
)

func NewSessionID() SessionID       { return newID[sessionIDT]("sess") }
func NewRunID() RunID               { return newID[runIDT]("run") }
func NewTurnID() TurnID             { return newID[turnIDT]("turn") }
func NewMessageID() MessageID       { return newID[messageIDT]("msg") }
func NewToolCallID() ToolCallID     { return newID[toolCallIDT]("tc") }
func NewEventID() EventID           { return newID[eventIDT]("evt") }
func NewArtifactID() ArtifactID     { return newID[artifactIDT]("art") }
func NewCheckpointID() CheckpointID { return newID[checkpointIDT]("ckpt") }

func ParseSessionID(s string) (SessionID, error)       { return ParseID[sessionIDT](s) }
func ParseRunID(s string) (RunID, error)               { return ParseID[runIDT](s) }
func ParseTurnID(s string) (TurnID, error)             { return ParseID[turnIDT](s) }
func ParseMessageID(s string) (MessageID, error)       { return ParseID[messageIDT](s) }
func ParseToolCallID(s string) (ToolCallID, error)     { return ParseID[toolCallIDT](s) }
func ParseEventID(s string) (EventID, error)           { return ParseID[eventIDT](s) }
func ParseArtifactID(s string) (ArtifactID, error)     { return ParseID[artifactIDT](s) }
func ParseCheckpointID(s string) (CheckpointID, error) { return ParseID[checkpointIDT](s) }

// HasPrefix reports whether the ID string starts with the given prefix.
func HasPrefix[T ~string](id ID[T], prefix string) bool {
	return strings.HasPrefix(string(id.val), prefix)
}
