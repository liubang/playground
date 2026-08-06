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
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Consolidator runs Phase 2: merges staged raw memories into MEMORY.md
// and regenerates memory_summary.md. This is the "compaction" step that
// keeps the memory handbook current and the summary concise.
type Consolidator struct {
	store     *Store
	model     domain.Model
	modelName string
}

// NewConsolidator creates a Phase 2 consolidator. modelName is the real
// model identifier to send in API requests.
func NewConsolidator(store *Store, model domain.Model, modelName string) *Consolidator {
	return &Consolidator{store: store, model: model, modelName: modelName}
}

// Consolidate merges new raw memories into MEMORY.md and regenerates
// memory_summary.md. Returns true if any changes were made.
//
// The consolidation workflow:
//  1. Diff the raw_memories.md file against the last git commit
//  2. If there is a diff, ask the model to merge new content into MEMORY.md
//  3. Ask the model to regenerate memory_summary.md from the updated MEMORY.md
//  4. Commit all changes
//
// If no model is available, falls back to a simple append strategy.
func (c *Consolidator) Consolidate(ctx context.Context) (bool, error) {
	// Check for new raw memories via git diff.
	diff, err := c.store.GitDiff(ctx)
	if err != nil {
		return false, fmt.Errorf("memory git diff: %w", err)
	}
	if strings.TrimSpace(diff) == "" {
		return false, nil // nothing new to consolidate
	}

	// Extract only the added lines from the diff — this is the new content
	// that hasn't been merged yet.
	newContent := extractAddedLines(diff)
	if strings.TrimSpace(newContent) == "" {
		return false, nil
	}

	// Read current state.
	currentMain, err := c.store.ReadMain()
	if err != nil {
		return false, fmt.Errorf("read MEMORY.md: %w", err)
	}

	var newMain string
	var newSummary string

	if c.model != nil {
		// Model-assisted consolidation.
		newMain, err = c.consolidateWithModel(ctx, currentMain, newContent)
		if err != nil || strings.TrimSpace(stripMarkdownFences(newMain)) == "" {
			// Fall back to simple append on model error or empty output.
			// An empty output would erase the entire MEMORY.md — we must
			// never allow that.
			newMain = appendMemory(currentMain, newContent)
		} else {
			// Strip potential markdown fences from model output.
			newMain = stripMarkdownFences(newMain)
		}

		// Regenerate summary from the updated main.
		newSummary, err = c.generateSummary(ctx, newMain)
		if err != nil || strings.TrimSpace(stripMarkdownFences(newSummary)) == "" {
			// Fall back to truncation on model error or empty output.
			newSummary = truncateToSummary(newMain)
		} else {
			newSummary = stripMarkdownFences(newSummary)
		}
	} else {
		// No model: simple append strategy.
		newMain = appendMemory(currentMain, newContent)
		newSummary = truncateToSummary(newMain)
	}

	// Scrub secrets before persisting (P3): existing MEMORY.md content may
	// predate transcript redaction and the model may echo it verbatim.
	newMain = RedactSecrets(newMain)
	newSummary = RedactSecrets(newSummary)

	// Write the updated files.
	if err := c.store.WriteMain(newMain); err != nil {
		return false, fmt.Errorf("write MEMORY.md: %w", err)
	}
	if err := c.store.WriteSummary(newSummary); err != nil {
		return false, fmt.Errorf("write memory_summary.md: %w", err)
	}

	// Commit the changes.
	if err := c.store.GitCommitAll(ctx, fmt.Sprintf("memory: consolidation at %s", time.Now().UTC().Format(time.RFC3339))); err != nil {
		return false, fmt.Errorf("git commit: %w", err)
	}

	return true, nil
}

// consolidateWithModel asks the model to merge new raw memories into
// the existing MEMORY.md content.
func (c *Consolidator) consolidateWithModel(ctx context.Context, currentMain, newContent string) (string, error) {
	systemPrompt := stageTwoSystemPrompt
	userPrompt := fmt.Sprintf(`Merge the new raw memories into the existing MEMORY.md.

## Existing MEMORY.md

%s

## New raw memories

%s

Produce the updated MEMORY.md following the rules in the system prompt.`, currentMain, newContent)

	req := domain.ModelRequest{
		ModelName:   c.modelName,
		Messages:    buildExtractionMessages(systemPrompt, userPrompt),
		MaxTokens:   8192,
		Temperature: 0.0,
	}

	stream, err := c.model.Stream(ctx, req)
	if err != nil {
		return "", fmt.Errorf("consolidation model call: %w", err)
	}
	defer stream.Close()

	var responseText strings.Builder
	for {
		event, err := stream.Recv()
		if err != nil {
			if err == io.EOF {
				break
			}
			return "", fmt.Errorf("consolidation stream error: %w", err)
		}
		if event.Kind == domain.ModelEventTextDelta {
			responseText.WriteString(event.TextDelta)
		}
	}

	return responseText.String(), nil
}

// generateSummary asks the model to produce a concise summary from
// the updated MEMORY.md.
func (c *Consolidator) generateSummary(ctx context.Context, mainContent string) (string, error) {
	systemPrompt := summarySystemPrompt
	userPrompt := fmt.Sprintf(`Produce a memory_summary.md from the following MEMORY.md content.
The summary must be under %d tokens (approximately %d characters).
Focus on the most actionable, high-signal information.

## MEMORY.md

%s`, SummaryTokenLimit, SummaryTokenLimit*4, mainContent)

	req := domain.ModelRequest{
		ModelName:   c.modelName,
		Messages:    buildExtractionMessages(systemPrompt, userPrompt),
		MaxTokens:   4096,
		Temperature: 0.0,
	}

	stream, err := c.model.Stream(ctx, req)
	if err != nil {
		return "", fmt.Errorf("summary model call: %w", err)
	}
	defer stream.Close()

	var responseText strings.Builder
	for {
		event, err := stream.Recv()
		if err != nil {
			if err == io.EOF {
				break
			}
			return "", fmt.Errorf("summary stream error: %w", err)
		}
		if event.Kind == domain.ModelEventTextDelta {
			responseText.WriteString(event.TextDelta)
		}
	}

	return responseText.String(), nil
}

// extractAddedLines parses a unified diff and returns only the added
// lines (those starting with '+', excluding '+++'/'---' file headers).
// We only skip "+++ " and "--- " (with a trailing space) which are the
// unified diff file headers (e.g. "--- a/file", "+++ b/file"). A content
// line like "+---" (a horizontal rule or YAML separator) must NOT be
// skipped — its text after the "+" is "---", which has no space.
func extractAddedLines(diff string) string {
	var b strings.Builder
	for _, line := range strings.Split(diff, "\n") {
		// Skip diff file headers: "+++ b/path" and "--- a/path".
		if strings.HasPrefix(line, "+++ ") || strings.HasPrefix(line, "--- ") {
			continue
		}
		if strings.HasPrefix(line, "+") && !strings.HasPrefix(line, "++") {
			b.WriteString(line[1:])
			b.WriteByte('\n')
		}
	}
	return b.String()
}

// stripMarkdownFences removes surrounding markdown code fences that
// models sometimes add (e.g. ```markdown ... ``` or ``` ... ```).
func stripMarkdownFences(s string) string {
	s = strings.TrimSpace(s)
	// Try ```markdown ... ```
	if strings.HasPrefix(s, "```markdown") && strings.HasSuffix(s, "```") {
		inner := strings.TrimPrefix(s, "```markdown")
		inner = strings.TrimSuffix(inner, "```")
		return strings.TrimSpace(inner)
	}
	// Try ```md ... ```
	if strings.HasPrefix(s, "```md") && strings.HasSuffix(s, "```") {
		inner := strings.TrimPrefix(s, "```md")
		inner = strings.TrimSuffix(inner, "```")
		return strings.TrimSpace(inner)
	}
	// Try generic ``` ... ```
	if strings.HasPrefix(s, "```") && strings.HasSuffix(s, "```") {
		inner := s[3:]
		// Skip optional language tag on first line.
		if idx := strings.IndexByte(inner, '\n'); idx >= 0 {
			inner = inner[idx+1:]
		}
		inner = strings.TrimSuffix(inner, "```")
		return strings.TrimSpace(inner)
	}
	return s
}

// appendMemory appends new content to MEMORY.md using a simple
// no-model strategy.
func appendMemory(currentMain, newContent string) string {
	var b strings.Builder
	b.WriteString(currentMain)
	if currentMain != "" && !strings.HasSuffix(currentMain, "\n") {
		b.WriteString("\n")
	}
	b.WriteString("\n---\n")
	b.WriteString(fmt.Sprintf("## Auto-merged at %s\n\n", time.Now().UTC().Format(time.RFC3339)))
	b.WriteString(newContent)
	return b.String()
}

// truncateToSummary creates a summary by truncating the main content.
// This is the fallback when no model is available.
func truncateToSummary(mainContent string) string {
	if mainContent == "" {
		return ""
	}
	// Try to use just the first section or first N chars.
	lines := strings.Split(mainContent, "\n")
	var summaryLines []string
	for _, line := range lines {
		if len(summaryLines) >= 100 {
			summaryLines = append(summaryLines, "\n(Memory truncated; use memory_read MEMORY.md for full content)")
			break
		}
		summaryLines = append(summaryLines, line)
	}
	return strings.Join(summaryLines, "\n")
}

// stageTwoSystemPrompt is the system prompt for Phase 2 consolidation.
// Adapted from Codex's stage_two_system.md.
const stageTwoSystemPrompt = `You are a Memory Consolidation Agent. Your job: merge new raw memories into the existing MEMORY.md handbook.

## Goal

Maintain a concise, well-organized memory handbook that:
- Preserves high-signal information from new sessions
- Removes redundancies and outdated entries
- Keeps entries categorized and searchable
- Stays under 32K characters total

## Rules

1. **Merge, don't just append** — integrate new info into existing sections
2. **Deduplicate** — if new info contradicts or supersedes old, replace the old
3. **Preserve evidence** — keep quoted preference signals and concrete facts
4. **Redact secrets** — remove any API keys, passwords, or tokens
5. **Structure** — use markdown headers and sections for navigability

## Sections to maintain

- ### User Preferences — coding style, tool choices, communication patterns
- ### Project Knowledge — architecture, dependencies, build commands
- ### Procedures — reusable workflows, debugging steps
- ### Pitfalls — known issues and how to avoid them
- ### Environment — tool versions, platform specifics

## When to forget

- Outdated information that has been explicitly superseded
- Highly specific one-time debugging details with no reusable value
- Information that is clearly wrong based on newer evidence

Output ONLY the updated MEMORY.md content, nothing else.`

// summarySystemPrompt is the system prompt for generating memory_summary.md.
const summarySystemPrompt = `You are a Memory Summary Agent. Your job: distill MEMORY.md into a concise summary.

## Rules

1. **Be concise** — the summary must fit within the token budget
2. **Prioritize actionable info** — preferences and procedures over facts
3. **Preserve key details** — specific tool versions, commands, and file paths
4. **Redact secrets** — never include API keys, passwords, or tokens
5. **Use markdown** — headers and bullet points for scanability

Output ONLY the summary content, nothing else.`
