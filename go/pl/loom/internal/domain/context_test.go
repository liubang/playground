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
	"encoding/json"
	"testing"
)

func TestContextManifestCanonicalizeDeterministic(t *testing.T) {
	draft := ContextManifest{
		Rules:         []ContextRuleRef{{Source: "CLAUDE.md", Hash: "sha256:rule"}},
		MessageRanges: []ContextMessageRange{{Sequence: 3, StartPart: 0, EndPart: 2}},
		CodeRefs: []ContextCodeRef{{
			Path:        "go/pl/loom/internal/domain/message.go",
			PathHash:    "sha256:path",
			ContentHash: "sha256:content",
			StartLine:   10,
			EndLine:     20,
		}},
		ArtifactRefs: []ContextArtifactRef{{
			ArtifactID:     NewArtifactID(),
			SummaryHash:    "sha256:summary",
			SummaryVersion: "v1",
		}},
		SummaryRefs:   []ContextSummaryRef{{ID: "summary-1", Hash: "sha256:summary-1", Lineage: []string{"summary-0"}}},
		Tokenizer:     TokenizerRef{Name: "tiktoken", Version: "1.0.0"},
		BudgetBuckets: []BudgetBucket{{Name: "rules", Tokens: 128}, {Name: "conversation", Tokens: 256}},
		Truncations:   []ContextTruncation{{Target: "tool_output", Reason: "token_budget"}},
		PromptHash:    "sha256:prompt",
	}

	manifest1, err := NewContextManifest(draft)
	if err != nil {
		t.Fatalf("NewContextManifest() error = %v", err)
	}
	manifest2, err := NewContextManifest(draft)
	if err != nil {
		t.Fatalf("NewContextManifest() second error = %v", err)
	}

	if manifest1.ID != manifest2.ID {
		t.Fatalf("ID mismatch: %q vs %q", manifest1.ID, manifest2.ID)
	}
	if manifest1.Hash != manifest2.Hash {
		t.Fatalf("hash mismatch: %q vs %q", manifest1.Hash, manifest2.Hash)
	}
	if err := manifest1.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}

	json1, err := manifest1.CanonicalJSON()
	if err != nil {
		t.Fatalf("CanonicalJSON() error = %v", err)
	}
	json2, err := manifest2.CanonicalJSON()
	if err != nil {
		t.Fatalf("CanonicalJSON() second error = %v", err)
	}
	if string(json1) != string(json2) {
		t.Fatalf("canonical json mismatch:\n%s\n%s", json1, json2)
	}

	var decoded ContextManifest
	if err := json.Unmarshal(json1, &decoded); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if decoded.ID != manifest1.ID || decoded.Hash != manifest1.Hash {
		t.Fatalf("decoded manifest mismatch: %+v vs %+v", decoded, manifest1)
	}
}

func TestContextManifestValidateErrors(t *testing.T) {
	manifest, err := NewContextManifest(ContextManifest{
		Tokenizer:  TokenizerRef{Name: "phase1"},
		PromptHash: "sha256:prompt",
	})
	if err != nil {
		t.Fatalf("NewContextManifest() error = %v", err)
	}
	manifest.Hash = "broken"
	if err := manifest.Validate(); err == nil {
		t.Fatal("expected hash mismatch error")
	}

	_, err = NewContextManifest(ContextManifest{PromptHash: "sha256:prompt"})
	if err == nil {
		t.Fatal("expected tokenizer validation error")
	}
}

func TestMarshalPayloadRoundTrip(t *testing.T) {
	msg := Message{
		ID:       NewMessageID(),
		Sequence: 1,
		Role:     RoleUser,
		Status:   MessageStatusFinal,
		Revision: 1,
		Parts:    []ContentPart{{PartIndex: 0, Kind: PartText, Text: "hello"}},
	}
	payload, err := MarshalPayload(MessageEventPayload{Message: msg})
	if err != nil {
		t.Fatalf("MarshalPayload() error = %v", err)
	}
	decoded, err := UnmarshalMessageEventPayload(payload)
	if err != nil {
		t.Fatalf("UnmarshalMessageEventPayload() error = %v", err)
	}
	if decoded.Message.ID != msg.ID || decoded.Message.Sequence != msg.Sequence {
		t.Fatalf("decoded message mismatch: %+v vs %+v", decoded.Message, msg)
	}
}
