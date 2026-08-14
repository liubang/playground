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
// Created: 2026/08/14

package domain

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
)

// EventModelRequestHeader records the full request header — model config,
// rendered system prompt, tool schemas, context manifest — the first time
// a loop instance issues a request and whenever it changes afterwards
// (docs/SURFACE_DESIGN.md §4.8). Together with the surface replayed from
// the log it answers "what exactly did the model see on call N" —
// model.request_started anchors each call to a header by hash.
const EventModelRequestHeader EventType = "model.request_header"

// RequestHeaderReason classifies why a header was logged.
type RequestHeaderReason string

const (
	// HeaderReasonInitial is the session's first header.
	HeaderReasonInitial RequestHeaderReason = "initial"
	// HeaderReasonResume is the first header of a loop instance attached
	// to a session that already has history (process restart, continue).
	HeaderReasonResume RequestHeaderReason = "resume"
	// HeaderReasonChange marks a header that differs from the previous one.
	HeaderReasonChange RequestHeaderReason = "change"
)

// RequestHeader is the complete non-transcript half of a model request.
type RequestHeader struct {
	ModelName   string        `json:"model_name"`
	Reasoning   ReasoningSpec `json:"reasoning"`
	MaxTokens   int64         `json:"max_tokens,omitempty"`
	Temperature float64       `json:"temperature,omitempty"`
	// System is the fully rendered system prompt (static + dynamic parts
	// as sent); empty when no prompt builder is configured.
	System string `json:"system,omitempty"`
	// Tools is the exact tool schema list exposed to the model.
	Tools []ToolDefinition `json:"tools,omitempty"`
	// Rules is the stable rule-set reference list from the context
	// manifest. The manifest's per-request parts (message ranges, budget
	// buckets, truncations) are deliberately excluded — they change with
	// every call and would defeat change detection; each call's full
	// manifest hash stays on model.request_started.
	Rules []ContextRuleRef `json:"rules,omitempty"`
}

// CanonicalHash derives the stable header identity used for change
// detection and request anchoring: SHA-256 over the canonical JSON
// encoding (Go's encoding/json sorts map keys, so the encoding is
// deterministic for the same content).
func (h RequestHeader) CanonicalHash() string {
	data, err := json.Marshal(h)
	if err != nil {
		return ""
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// RequestHeaderPayload is the EventModelRequestHeader payload.
type RequestHeaderPayload struct {
	Header RequestHeader       `json:"header"`
	Reason RequestHeaderReason `json:"reason"`
	Hash   string              `json:"hash"`
}
