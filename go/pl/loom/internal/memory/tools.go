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
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"regexp"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// ToolMemory is the single memory tool: one definition, four actions
// (list|read|search|add_note), risk graded per action.
const ToolMemory = "memory"

// Memory tool actions.
const (
	ActionList    = "list"
	ActionRead    = "read"
	ActionSearch  = "search"
	ActionAddNote = "add_note"
)

// Limits.
const (
	DefaultListMaxResults   = 200
	DefaultSearchMaxResults = 200
	// DefaultReadMaxLines bounds a read that passes no max_lines: memory
	// files only grow, and an uncapped full read injects the whole store
	// into context in one shot (observed: a 34KB MEMORY.md single read).
	// The truncated hint tells the model how to page for the rest.
	DefaultReadMaxLines = 200
	SummaryTokenLimit   = 2500
)

// noteFilePattern validates ad-hoc note filenames.
var noteFilePattern = regexp.MustCompile(`^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}-[a-z0-9][a-z0-9-]{0,79}\.md$`)

// noteSlugPattern validates a bare note slug; the current UTC timestamp is
// prepended automatically so the model never has to look up the date
// (observed in live transcripts: an extra run_cmd date call per add_note).
var noteSlugPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{0,79}\.md$`)

// timestampedNoteFilename renders the canonical note filename for a slug.
func timestampedNoteFilename(slug string) string {
	return time.Now().UTC().Format("2006-01-02T15-04-05") + "-" + slug + ".md"
}

// resolveNoteFilename maps a validated filename onto the final note name,
// prepending the current UTC timestamp to bare slugs. It runs at Execute
// time, never Prepare: a timestamp baked into the signed canonical
// arguments cannot survive the freshness re-Prepare, which runs after
// approval — at a later second — and would mint a different name, so
// every slug-form add_note would fail closed as a security error.
func resolveNoteFilename(filename string) (string, error) {
	switch {
	case filename == "":
		return timestampedNoteFilename("note"), nil
	case noteFilePattern.MatchString(filename):
		// Fully timestamped already; use as-is.
		return filename, nil
	case noteSlugPattern.MatchString(filename):
		return timestampedNoteFilename(strings.TrimSuffix(filename, ".md")), nil
	default:
		return "", domain.NewError(domain.ErrInvalidInput,
			"filename must be a slug like \"data-prefs.md\" (a UTC timestamp is prepended automatically) or fully timestamped YYYY-MM-DDTHH-MM-SS-slug.md")
	}
}

// memoryArgs is the model-visible schema of the unified memory tool:
// action selects the operation, the remaining fields are per-action.
type memoryArgs struct {
	Action     string `json:"action"`
	Path       string `json:"path,omitempty"`
	Query      string `json:"query,omitempty"`
	Filename   string `json:"filename,omitempty"`
	Note       string `json:"note,omitempty"`
	LineOffset int    `json:"line_offset,omitempty"`
	MaxLines   int    `json:"max_lines,omitempty"`
	MaxResults int    `json:"max_results,omitempty"`
}

// MemoryTool is the unified memory tool. The read actions (list, read,
// search) are R1; add_note writes to the store and is R2. The definition
// stays capability-free (static R0) so the per-action tier never sits
// below the definition default — the agent loop's prepared-call drift
// check fails closed on that (the browser/exec_session pattern).
type MemoryTool struct {
	def   domain.ToolDefinition
	store *Store
}

// NewMemoryTool creates the memory tool.
func NewMemoryTool(store *Store) (*MemoryTool, error) {
	if store == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "memory requires a non-nil store")
	}
	def := domain.ToolDefinition{
		Name: ToolMemory,
		Description: "Read and update the persistent memory store. " +
			"Actions: 'list' enumerates files and directories under path (default: root) — explore the memory " +
			"hierarchy before reading; 'read' reads the memory file at path (required), paginating with " +
			"line_offset/max_lines; 'search' finds substring matches for query (required) across memory files — " +
			"use it before answering questions about prior work, user preferences, or project conventions; " +
			"'add_note' appends a timestamped markdown note (note required) after the user explicitly " +
			"asks to remember, forget, or update something — it is consolidated into the main memory later.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["list","read","search","add_note"],"description":"The memory operation."},"path":{"type":"string","description":"Relative path within the memory store (list: default root; read: required)."},"query":{"type":"string","minLength":1,"maxLength":512,"description":"Search substring (search; required)."},"filename":{"type":"string","maxLength":128,"description":"Note filename slug, e.g. 'data-prefs.md' (add_note; optional, default 'note.md'). The current UTC timestamp YYYY-MM-DDTHH-MM-SS- is prepended automatically — no date lookup needed. A fully timestamped name is also accepted."},"note":{"type":"string","minLength":1,"maxLength":4096,"description":"The memory note content (add_note; required)."},"line_offset":{"type":"integer","minimum":1,"description":"1-indexed line offset to start reading from (read)."},"max_lines":{"type":"integer","minimum":1,"maximum":2000,"description":"Maximum lines to return (read; default 200 — paginate with line_offset for more)."},"max_results":{"type":"integer","minimum":1,"maximum":2000,"description":"Maximum entries/matches to return (list/search; default 200)."}},"required":["action"]}`),
		Source:      domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &MemoryTool{def: def, store: store}, nil
}

func (t *MemoryTool) Definition() domain.ToolDefinition { return t.def }
func (t *MemoryTool) ConcurrentSafe() bool              { return true }

// validate normalizes the call and reports the per-action risk: add_note
// writes (R2), everything else reads (R1).
func (args *memoryArgs) validate() (domain.RiskLevel, error) {
	switch args.Action {
	case ActionList:
		if args.MaxResults <= 0 {
			args.MaxResults = DefaultListMaxResults
		}
		return domain.R1, nil
	case ActionRead:
		if args.Path == "" {
			return 0, domain.NewError(domain.ErrInvalidInput, "path is required for action=read")
		}
		return domain.R1, nil
	case ActionSearch:
		if strings.TrimSpace(args.Query) == "" {
			return 0, domain.NewError(domain.ErrInvalidInput, "query is required for action=search")
		}
		if args.MaxResults <= 0 {
			args.MaxResults = DefaultSearchMaxResults
		}
		return domain.R1, nil
	case ActionAddNote:
		args.Note = strings.TrimSpace(args.Note)
		if args.Note == "" {
			return 0, domain.NewError(domain.ErrInvalidInput, "note is required for action=add_note")
		}
		// Shape-check only: the timestamp is prepended at Execute time
		// (resolveNoteFilename) so the signed canonical arguments stay
		// stable across the freshness re-Prepare. The empty filename
		// defaults to the bare slug — a deterministic, idempotent fill.
		switch {
		case args.Filename == "":
			args.Filename = "note.md"
		case noteFilePattern.MatchString(args.Filename), noteSlugPattern.MatchString(args.Filename):
		default:
			return 0, domain.NewError(domain.ErrInvalidInput,
				"filename must be a slug like \"data-prefs.md\" (a UTC timestamp is prepended automatically) or fully timestamped YYYY-MM-DDTHH-MM-SS-slug.md")
		}
		return domain.R2, nil
	default:
		return 0, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("unknown action %q (want list|read|search|add_note)", args.Action))
	}
}

func (args memoryArgs) approvalDesc() string {
	switch args.Action {
	case ActionList:
		return fmt.Sprintf("List memory files under %s", args.Path)
	case ActionRead:
		return fmt.Sprintf("Read memory file %s", args.Path)
	case ActionSearch:
		return fmt.Sprintf("Search memory for %q", toolkit.Ellipsize(args.Query, 40))
	default:
		return fmt.Sprintf("Add memory note %s", args.Filename)
	}
}

func (t *MemoryTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeLenient[memoryArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid memory arguments", domain.WithCause(err))
	}
	risk, err := args.validate()
	if err != nil {
		return domain.PreparedCall{}, err
	}
	canonical, _ := json.Marshal(args)
	// validate mutates the args (add_note filename defaulting, max_results
	// defaults), so the canonical form — not the model's raw JSON — is
	// what Execute must decode.
	call.Arguments = canonical
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         risk,
		ApprovalDesc: args.approvalDesc(),
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

func (t *MemoryTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	var args memoryArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}

	var payload map[string]any
	var err error
	switch args.Action {
	case ActionList:
		var entries any
		maxResults := args.MaxResults
		if maxResults <= 0 {
			maxResults = DefaultListMaxResults
		}
		entries, err = t.store.List(args.Path, maxResults)
		payload = map[string]any{"entries": entries}
	case ActionRead:
		maxLines := args.MaxLines
		if maxLines <= 0 {
			maxLines = DefaultReadMaxLines
		}
		var content string
		var total int
		content, total, err = t.store.ReadFile(args.Path, args.LineOffset, maxLines)
		if errors.Is(err, fs.ErrNotExist) {
			// Replace the raw os.PathError (which leaks the store's
			// absolute root) with actionable guidance.
			err = domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("memory file not found: %q — nothing is stored there; use action=list to browse what exists", args.Path))
		}
		start := args.LineOffset
		if start < 1 {
			start = 1
		}
		payload = map[string]any{"content": content, "total_lines": total}
		if end := start - 1 + countLines(content); content != "" && end < total {
			payload["truncated"] = true
			payload["hint"] = fmt.Sprintf(
				"showing lines %d-%d of %d; continue with line_offset=%d (or pass a larger max_lines, up to 2000)",
				start, end, total, end+1,
			)
		}
	case ActionSearch:
		var matches any
		maxResults := args.MaxResults
		if maxResults <= 0 {
			maxResults = DefaultSearchMaxResults
		}
		matches, err = t.store.Search(args.Query, maxResults)
		payload = map[string]any{"matches": matches}
	case ActionAddNote:
		var filename string
		filename, err = resolveNoteFilename(args.Filename)
		if err == nil {
			err = t.store.AddNote(filename, args.Note)
		}
		payload = map[string]any{
			"path":   NotesDir + "/" + filename,
			"status": "created",
		}
	default:
		err = domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("unknown action %q (want list|read|search|add_note)", args.Action))
	}
	if err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	raw, _ := json.Marshal(payload)
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

// countLines returns the number of lines in a non-empty read chunk.
func countLines(content string) int {
	if content == "" {
		return 0
	}
	return strings.Count(content, "\n") + 1
}

func memoryToolError(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	code, message := "internal", err.Error()
	retryable := true
	var agentErr *domain.AgentError
	if errors.As(err, &agentErr) {
		code, message = string(agentErr.Code), agentErr.Message
		// Invalid input errors are not retryable — the same call will
		// fail the same way.
		if agentErr.Code == domain.ErrInvalidInput {
			retryable = false
		}
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusError,
		Error:      &domain.ToolError{Code: code, Message: message, Retryable: retryable},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}
