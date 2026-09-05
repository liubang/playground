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
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// Tool names.
const (
	ToolList    = "memory_list"
	ToolRead    = "memory_read"
	ToolSearch  = "memory_search"
	ToolAddNote = "memory_add_note"
)

// Limits.
const (
	DefaultListMaxResults   = 200
	DefaultSearchMaxResults = 200
	DefaultReadMaxTokens    = 20000
	SummaryTokenLimit       = 2500
)

// noteFilePattern validates ad-hoc note filenames.
var noteFilePattern = regexp.MustCompile(`^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}-[a-z0-9][a-z0-9-]{0,79}\.md$`)

// ---------------------------------------------------------------------------
// memory_list
// ---------------------------------------------------------------------------

// ListTool lists files and directories in the memory store.
type ListTool struct {
	def   domain.ToolDefinition
	store *Store
}

// NewListTool creates the memory_list tool.
func NewListTool(store *Store) (*ListTool, error) {
	if store == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "memory_list requires a non-nil store")
	}
	def := domain.ToolDefinition{
		Name: ToolList,
		Description: "List files and directories under a path in the memory store. " +
			"Use this to explore the memory hierarchy before reading specific files.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","description":"Relative path within the memory store. Default: root."},"max_results":{"type":"integer","minimum":1,"maximum":2000,"description":"Maximum entries to return. Default: 200."}}}`),
		Source:      domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &ListTool{def: def, store: store}, nil
}

func (t *ListTool) Definition() domain.ToolDefinition { return t.def }
func (t *ListTool) ConcurrentSafe() bool              { return true }

type listArgs struct {
	Path       string `json:"path"`
	MaxResults int    `json:"max_results"`
}

func (t *ListTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	var args listArgs
	dec := json.NewDecoder(bytes.NewReader(call.Arguments))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid memory_list arguments", domain.WithCause(err))
	}
	if args.MaxResults <= 0 {
		args.MaxResults = DefaultListMaxResults
	}
	canonical, _ := json.Marshal(args)
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: fmt.Sprintf("List memory files under %s", args.Path),
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

func (t *ListTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	var args listArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	maxResults := args.MaxResults
	if maxResults <= 0 {
		maxResults = DefaultListMaxResults
	}
	entries, err := t.store.List(args.Path, maxResults)
	if err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	raw, _ := json.Marshal(map[string]any{"entries": entries})
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

// ---------------------------------------------------------------------------
// memory_read
// ---------------------------------------------------------------------------

// ReadTool reads a file from the memory store.
type ReadTool struct {
	def   domain.ToolDefinition
	store *Store
}

// NewReadTool creates the memory_read tool.
func NewReadTool(store *Store) (*ReadTool, error) {
	if store == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "memory_read requires a non-nil store")
	}
	def := domain.ToolDefinition{
		Name: ToolRead,
		Description: "Read a memory file by relative path, optionally starting at a line offset " +
			"and limiting the number of lines returned. Use this to read MEMORY.md, rollout summaries, or skill files.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","description":"Relative path within the memory store."},"line_offset":{"type":"integer","minimum":1,"description":"1-indexed line offset to start reading from."},"max_lines":{"type":"integer","minimum":1,"maximum":2000,"description":"Maximum lines to return. Default: all."}},"required":["path"]}`),
		Source:      domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &ReadTool{def: def, store: store}, nil
}

func (t *ReadTool) Definition() domain.ToolDefinition { return t.def }
func (t *ReadTool) ConcurrentSafe() bool              { return true }

type readArgs struct {
	Path       string `json:"path"`
	LineOffset int    `json:"line_offset"`
	MaxLines   int    `json:"max_lines"`
}

func (t *ReadTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	var args readArgs
	dec := json.NewDecoder(bytes.NewReader(call.Arguments))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid memory_read arguments", domain.WithCause(err))
	}
	if args.Path == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	canonical, _ := json.Marshal(args)
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: fmt.Sprintf("Read memory file %s", args.Path),
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

func (t *ReadTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	var args readArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	content, total, err := t.store.ReadFile(args.Path, args.LineOffset, args.MaxLines)
	if err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	raw, _ := json.Marshal(map[string]any{"content": content, "total_lines": total})
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

// ---------------------------------------------------------------------------
// memory_search
// ---------------------------------------------------------------------------

// SearchTool searches memory files for substring matches.
type SearchTool struct {
	def   domain.ToolDefinition
	store *Store
}

// NewSearchTool creates the memory_search tool.
func NewSearchTool(store *Store) (*SearchTool, error) {
	if store == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "memory_search requires a non-nil store")
	}
	def := domain.ToolDefinition{
		Name: ToolSearch,
		Description: "Search memory files for substring matches. Use this to find relevant memories " +
			"before answering questions about prior work, user preferences, or project conventions.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"query":{"type":"string","minLength":1,"maxLength":512,"description":"Search substring."},"max_results":{"type":"integer","minimum":1,"maximum":2000,"description":"Maximum matches to return. Default: 200."}},"required":["query"]}`),
		Source:      domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &SearchTool{def: def, store: store}, nil
}

func (t *SearchTool) Definition() domain.ToolDefinition { return t.def }
func (t *SearchTool) ConcurrentSafe() bool              { return true }

type searchArgs struct {
	Query      string `json:"query"`
	MaxResults int    `json:"max_results"`
}

func (t *SearchTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	var args searchArgs
	dec := json.NewDecoder(bytes.NewReader(call.Arguments))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid memory_search arguments", domain.WithCause(err))
	}
	if strings.TrimSpace(args.Query) == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "query is required")
	}
	if args.MaxResults <= 0 {
		args.MaxResults = DefaultSearchMaxResults
	}
	canonical, _ := json.Marshal(args)
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: fmt.Sprintf("Search memory for %q", toolkit.Ellipsize(args.Query, 40)),
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

func (t *SearchTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	var args searchArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	maxResults := args.MaxResults
	if maxResults <= 0 {
		maxResults = DefaultSearchMaxResults
	}
	matches, err := t.store.Search(args.Query, maxResults)
	if err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	raw, _ := json.Marshal(map[string]any{"matches": matches})
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

// ---------------------------------------------------------------------------
// memory_add_note
// ---------------------------------------------------------------------------

// AddNoteTool creates an ad-hoc memory note when the user explicitly asks
// to remember something.
type AddNoteTool struct {
	def   domain.ToolDefinition
	store *Store
}

// NewAddNoteTool creates the memory_add_note tool.
func NewAddNoteTool(store *Store) (*AddNoteTool, error) {
	if store == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "memory_add_note requires a non-nil store")
	}
	def := domain.ToolDefinition{
		Name: ToolAddNote,
		Description: "Create an append-only ad-hoc memory note after the user explicitly asks to " +
			"remember, forget, or update something. The note is stored as a timestamped markdown file " +
			"and will be consolidated into the main memory during the next consolidation pass.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"filename":{"type":"string","pattern":"^\\d{4}-\\d{2}-\\d{2}T\\d{2}-\\d{2}-\\d{2}-[a-z0-9][a-z0-9-]{0,79}\\.md$","minLength":24,"maxLength":128,"description":"Timestamped slug filename: YYYY-MM-DDTHH-MM-SS-slug.md"},"note":{"type":"string","minLength":1,"maxLength":4096,"description":"The memory note content."}},"required":["filename","note"]}`),
		Source:      domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &AddNoteTool{def: def, store: store}, nil
}

func (t *AddNoteTool) Definition() domain.ToolDefinition { return t.def }
func (t *AddNoteTool) ConcurrentSafe() bool              { return true }

type addNoteArgs struct {
	Filename string `json:"filename"`
	Note     string `json:"note"`
}

func (t *AddNoteTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	var args addNoteArgs
	dec := json.NewDecoder(bytes.NewReader(call.Arguments))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid memory_add_note arguments", domain.WithCause(err))
	}
	if !noteFilePattern.MatchString(args.Filename) {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput,
			"filename must match YYYY-MM-DDTHH-MM-SS-slug.md pattern")
	}
	args.Note = strings.TrimSpace(args.Note)
	if args.Note == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "note is required")
	}
	canonical, _ := json.Marshal(args)
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R2,
		ApprovalDesc: fmt.Sprintf("Add memory note %s", args.Filename),
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

func (t *AddNoteTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	var args addNoteArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	if err := t.store.AddNote(args.Filename, args.Note); err != nil {
		return memoryToolError(prepared.Call.ID, startedAt, err)
	}
	raw, _ := json.Marshal(map[string]any{
		"path":   NotesDir + "/" + args.Filename,
		"status": "created",
	})
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
