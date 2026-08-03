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
// Created: 2026/08/02

package memory

import (
	"context"
	"strings"
	"testing"
)

func TestPromptProviderEmptySummary(t *testing.T) {
	s := newTestStore(t)
	p := NewPromptProvider(s)
	got, err := p.MemoryPrompt(context.Background())
	if err != nil {
		t.Fatalf("MemoryPrompt: %v", err)
	}
	if got != "" {
		t.Errorf("expected empty prompt, got %q", got)
	}
}

func TestPromptProviderWithSummary(t *testing.T) {
	s := newTestStore(t)
	summary := "# Summary\n\nUser prefers Go."
	if err := s.WriteSummary(summary); err != nil {
		t.Fatalf("WriteSummary: %v", err)
	}
	p := NewPromptProvider(s)
	got, err := p.MemoryPrompt(context.Background())
	if err != nil {
		t.Fatalf("MemoryPrompt: %v", err)
	}
	if got != summary {
		t.Errorf("MemoryPrompt() = %q, want %q", got, summary)
	}
}

func TestPromptProviderTruncation(t *testing.T) {
	s := newTestStore(t)
	// Create a summary longer than the token limit.
	longSummary := strings.Repeat("x", SummaryTokenLimit*4+1000)
	if err := s.WriteSummary(longSummary); err != nil {
		t.Fatalf("WriteSummary: %v", err)
	}
	p := NewPromptProvider(s)
	got, err := p.MemoryPrompt(context.Background())
	if err != nil {
		t.Fatalf("MemoryPrompt: %v", err)
	}
	maxChars := SummaryTokenLimit * 4
	if len(got) > maxChars+100 { // allow some slack for truncation message
		t.Errorf("MemoryPrompt() len = %d, expected <= %d", len(got), maxChars+100)
	}
	if !strings.Contains(got, "truncated") {
		t.Error("expected truncation notice in output")
	}
}

func TestPromptProviderRuleRef(t *testing.T) {
	s := newTestStore(t)
	p := NewPromptProvider(s)
	ref := p.RuleRef("injected memory section")
	if ref.Source != "loom://memory/summary" {
		t.Errorf("Source = %q, want loom://memory/summary", ref.Source)
	}
	// The context manifest rejects rules with an empty hash.
	if !strings.HasPrefix(ref.Hash, "sha256:") || len(ref.Hash) != len("sha256:")+64 {
		t.Errorf("Hash = %q, want sha256:<64 hex chars>", ref.Hash)
	}
	// The hash is content-addressed: same content, same hash; different
	// content, different hash.
	if again := p.RuleRef("injected memory section"); again.Hash != ref.Hash {
		t.Errorf("RuleRef not deterministic: %q vs %q", again.Hash, ref.Hash)
	}
	if other := p.RuleRef("different content"); other.Hash == ref.Hash {
		t.Error("RuleRef hash should change with content")
	}
}

func TestMemoryInstructionsContainsToolNames(t *testing.T) {
	for _, name := range []string{"memory_search", "memory_read", "memory_add_note"} {
		if !strings.Contains(MemoryInstructions, name) {
			t.Errorf("MemoryInstructions missing tool name %q", name)
		}
	}
}
