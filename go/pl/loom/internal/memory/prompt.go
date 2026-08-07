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
	"crypto/sha256"
	"encoding/hex"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PromptProvider injects memory_summary.md into the system prompt,
// following the Codex progressive disclosure pattern: only the summary
// is injected (truncated to SummaryTokenLimit tokens); the agent uses
// memory tools to access details on demand.
type PromptProvider struct {
	store *Store
}

// NewPromptProvider creates a prompt provider backed by the memory store.
func NewPromptProvider(store *Store) *PromptProvider {
	return &PromptProvider{store: store}
}

// MemoryPrompt returns the memory section for the system prompt, or empty
// string if no memory summary exists. The content is truncated to
// approximately SummaryTokenLimit tokens.
func (p *PromptProvider) MemoryPrompt(ctx context.Context) (string, error) {
	summary, err := p.store.ReadSummary()
	if err != nil {
		return "", nil // degrade gracefully
	}
	summary = strings.TrimSpace(summary)
	if summary == "" {
		return "", nil
	}
	// Truncate to ~SummaryTokenLimit tokens (rough approximation: 4 chars/token).
	maxChars := SummaryTokenLimit * 4
	if len(summary) > maxChars {
		summary = summary[:maxChars] + "\n(Memory summary truncated; use memory_search and memory_read for full content)"
	}
	return summary, nil
}

// RuleRef returns the context rule reference for audit purposes. The hash
// is computed over the exact injected memory section content so the context
// manifest can detect drift between turns.
func (p *PromptProvider) RuleRef(content string) domain.ContextRuleRef {
	sum := sha256.Sum256([]byte(content))
	return domain.ContextRuleRef{
		Source: "loom://memory/summary",
		Hash:   "sha256:" + hex.EncodeToString(sum[:]),
	}
}

// MemoryInstructions is the developer instructions appended when memory
// tools are active. It tells the model how to use the memory system.
const MemoryInstructions = `You have access to a persistent memory system that stores learnings from past sessions. Use it to provide better, more personalized assistance.

## Memory layout

- ` + "`memory_summary.md`" + ` — injected below (hot tier, refreshed every turn)
- ` + "`MEMORY.md`" + ` — searchable handbook with structured entries (warm tier)
- ` + "`rollout_summaries/`" + ` — per-session recaps (cold tier)
- ` + "`skills/`" + ` — reusable procedures (cold tier)
- ` + "`extensions/ad_hoc/notes/`" + ` — user-flagged notes (cold tier)

## When to use memory

- **Always check** when the user references past work, preferences, or project conventions
- **Search first** with ` + "`memory_search`" + ` before answering questions about prior context
- **Read details** with ` + "`memory_read`" + ` when search results are relevant
- **Quick pass**: skim the summary above, search MEMORY.md with keywords, open 1-2 relevant files

## When to skip memory

- Clearly self-contained requests (e.g., "what is 2+2", "explain recursion")
- When the user explicitly says to ignore past context

## Updating memory

Only update memory when the user **explicitly asks** to remember, forget, or update something.
Use ` + "`memory_add_note`" + ` to create a timestamped note. Do NOT update memory files directly.`
