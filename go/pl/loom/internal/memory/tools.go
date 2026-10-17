// Copyright (c) 2026 The Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

const (
	ToolMemory              = "memory"
	ActionList              = "list"
	ActionRead              = "read"
	ActionSearch            = "search"
	ActionRemember          = "remember"
	ActionForget            = "forget"
	ActionAddNote           = "add_note" // Input compatibility; normalized to remember.
	DefaultListMaxResults   = 50
	DefaultSearchMaxResults = 20
	DefaultReadMaxLines     = 100
	SummaryTokenLimit       = 2500
	maxToolBytes            = 32 * 1024
)

type memoryArgs struct {
	Action     string `json:"action"`
	ID         string `json:"id,omitempty"`
	Revision   int64  `json:"revision,omitzero"`
	Scope      string `json:"scope,omitempty"`
	Content    string `json:"content,omitempty"`
	Query      string `json:"query,omitempty"`
	Note       string `json:"note,omitempty"`
	Filename   string `json:"filename,omitempty"`
	ByteOffset int    `json:"byte_offset,omitzero"`
	LineOffset int    `json:"line_offset,omitzero"`
	MaxLines   int    `json:"max_lines,omitzero"`
	MaxResults int    `json:"max_results,omitzero"`
}

// MemoryTool exposes active records only. Prepare normalizes and signs the
// mutation, while the host-bound Store and ToolContext supply scope and
// source attribution. All actions run without approval: memory is advisory
// context (recoverable by restating a fact), and the background extraction
// pipeline already writes to the same store unattended.
type MemoryTool struct {
	def   domain.ToolDefinition
	store *Store
}

func NewMemoryTool(store *Store) (*MemoryTool, error) {
	if store == nil || store.repo == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "memory requires a repository")
	}
	def := domain.ToolDefinition{
		Name:        ToolMemory,
		Description: "Persistent cross-session memory. list returns active IDs/revisions; read reads an ID; search finds evidence before answering questions about prior work. remember stores an explicitly requested fact immediately (scope=global for personal preferences, workspace for project facts). To update, pass the existing id and revision. forget removes the specified id/revision immediately. Replacement/forget also clears automatic evidence in the affected scope; global changes affect all workspaces. Never claim to remember or forget unless this tool succeeds. add_note is a compatibility alias for remember.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["list","read","search","remember","forget","add_note"]},"id":{"type":"string","description":"Active record ID (read/forget required; remember optional update)."},"revision":{"type":"integer","minimum":1,"description":"Current revision from read/list; required for update/forget."},"scope":{"type":"string","enum":["global","workspace"],"description":"Remember scope: default global. For update/forget use the scope of the target."},"content":{"type":"string","minLength":1,"maxLength":4096},"query":{"type":"string","minLength":1,"maxLength":512},"note":{"type":"string","description":"Legacy alias for content."},"filename":{"type":"string","description":"Legacy label accepted by add_note; not a storage path."},"byte_offset":{"type":"integer","minimum":0,"description":"Continue a byte-limited read using next_byte_offset."},"line_offset":{"type":"integer","minimum":1},"max_lines":{"type":"integer","minimum":1,"maximum":500},"max_results":{"type":"integer","minimum":1,"maximum":200}},"required":["action"]}`),
		Source:      domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, err
	}
	return &MemoryTool{def: def, store: store}, nil
}

func (t *MemoryTool) Definition() domain.ToolDefinition { return t.def }
func (t *MemoryTool) ConcurrentSafe() bool              { return true }

func (a *memoryArgs) validate() (domain.RiskLevel, error) {
	invalid := func(message string) (domain.RiskLevel, error) {
		return 0, domain.NewError(domain.ErrInvalidInput, message)
	}
	if a.Action == ActionAddNote {
		a.Action = ActionRemember
	}
	if a.Note != "" {
		if a.Content != "" && a.Content != a.Note {
			return invalid("content and note disagree")
		}
		a.Content, a.Note = a.Note, ""
	}
	// Compatibility labels carry no persistence identity; tool call IDs do.
	a.Filename = ""
	a.Content = strings.TrimSpace(RedactSecrets(a.Content))
	a.Query = strings.TrimSpace(a.Query)
	if a.Scope != "" && a.Scope != "global" && a.Scope != "workspace" {
		return invalid("scope must be global or workspace")
	}
	if a.MaxResults < 0 || a.MaxResults > 200 || a.MaxLines < 0 || a.MaxLines > 500 || a.LineOffset < 0 || a.ByteOffset < 0 || a.Revision < 0 {
		return invalid("pagination or revision is out of range")
	}
	switch a.Action {
	case ActionList, ActionSearch:
		if a.Action == ActionSearch && (a.Query == "" || len(a.Query) > MaxQueryBytes) {
			return invalid("search query must be between 1 and 512 bytes")
		}
		if a.MaxResults == 0 {
			a.MaxResults = DefaultListMaxResults
			if a.Action == ActionSearch {
				a.MaxResults = DefaultSearchMaxResults
			}
		}
		return domain.R1, nil
	case ActionRead:
		if a.ID == "" {
			return invalid("read requires an ID; use list or search first")
		}
		if a.LineOffset == 0 {
			a.LineOffset = 1
		}
		if a.MaxLines == 0 {
			a.MaxLines = DefaultReadMaxLines
		}
		return domain.R1, nil
	case ActionRemember, ActionForget:
		if a.Scope == "" {
			a.Scope = "global"
		}
		if a.ID != "" && a.Revision < 1 {
			return invalid("update/forget requires the current revision")
		}
		if a.Action == ActionForget {
			if a.ID == "" || a.Content != "" {
				return invalid("forget requires an ID and no content")
			}
		} else if a.Content == "" || len(a.Content) > MaxContentBytes || !utf8.ValidString(a.Content) {
			return invalid("content must be valid UTF-8 and between 1 and 4096 bytes")
		}
		return domain.R2, nil
	default:
		return invalid("unknown memory action")
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
	if args.Scope == "workspace" && t.store.workspace == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "no workspace is bound to this memory tool")
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	call.Arguments = canonical
	description := fmt.Sprintf("Memory %s (%s): %s", args.Action, args.Scope, toolkit.Ellipsize(args.Content+args.ID+args.Query, 120))
	return domain.PreparedCall{Call: call, Definition: t.def, Risk: risk, ApprovalDesc: description, ArgsHash: toolkit.ArgsFingerprint(canonical)}, nil
}

func (t *MemoryTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	started := time.Now()
	args, err := toolkit.DecodeLenient[memoryArgs](prepared.Call.Arguments)
	if err == nil {
		_, err = args.validate()
	}
	var payload any
	if err == nil {
		switch args.Action {
		case ActionList:
			var result SearchResult
			result, err = t.store.repo.QueryMemory(ctx, t.store.workspace, "", args.MaxResults)
			if err != nil {
				break
			}
			for i := range result.Records {
				// Listing IDs is cheap; full evidence is retrieved on demand.
				result.Records[i].Content = utf8Prefix(result.Records[i].Content, 240)
				result.Records[i].Rollout = ""
			}
			payload, err = boundedRecords(result)
		case ActionRead:
			var record Record
			record, err = t.store.Read(ctx, args.ID)
			if err != nil {
				break
			}
			text := record.Content
			if record.Rollout != "" {
				text += "\n\n" + record.Rollout
			}
			lines := strings.Split(text, "\n")
			start := min(args.LineOffset-1, len(lines))
			end := min(start+args.MaxLines, len(lines))
			byteStart := 0
			for _, line := range lines[:start] {
				byteStart += len(line) + 1
			}
			byteStart = min(byteStart, len(text))
			chunk := strings.Join(lines[start:end], "\n")
			if args.ByteOffset > 0 {
				byteStart = min(args.ByteOffset, len(text))
				if !utf8.ValidString(text[byteStart:]) {
					err = domain.NewError(domain.ErrInvalidInput, "byte_offset must be a UTF-8 boundary")
					break
				}
				chunk = text[byteStart:]
			}
			record.Content, record.Rollout = "", ""
			page := map[string]any{"record": record, "content": "", "total_lines": len(lines), "total_bytes": len(text), "truncated": true, "next_byte_offset": byteStart}
			if len(chunk) <= maxToolBytes/8 && args.ByteOffset == 0 {
				page["next_line_offset"] = end + 1
			}
			chunk = utf8Prefix(chunk, maxToolBytes/8)
			page["content"] = chunk
			page["next_byte_offset"] = byteStart + len(chunk)
			page["truncated"] = byteStart+len(chunk) < len(text)
			payload = page
		case ActionSearch:
			var result SearchResult
			result, err = t.store.Search(ctx, args.Query, args.MaxResults)
			if err != nil {
				break
			}
			for i := range result.Records {
				result.Records[i].Content = utf8Prefix(result.Records[i].Content, 512)
				result.Records[i].Rollout = ""
			}
			payload, err = boundedRecords(result)
		case ActionRemember, ActionForget:
			scope := ""
			if args.Scope == "workspace" {
				scope = t.store.workspace
			}
			origin := domain.ToolContextFrom(ctx)
			var record Record
			record, err = t.store.Mutate(ctx, Mutation{RequestID: prepared.Call.ID.String(), ID: args.ID, Revision: args.Revision, Scope: scope, Content: args.Content, Forget: args.Action == ActionForget, SessionID: origin.SessionID, SourceVersion: origin.SourceVersion})
			if err != nil {
				break
			}
			result := map[string]any{"record": record, "status": args.Action, "visible_immediately": true}
			if args.ID != "" && record.Revision > args.Revision {
				revocation := "workspace"
				if scope == "" {
					revocation = "all_workspaces"
				}
				result["automatic_evidence_revoked"] = revocation
			}
			payload = result
		}
	}
	if err != nil {
		return memoryToolError(prepared.Call.ID, started, err)
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		return memoryToolError(prepared.Call.ID, started, err)
	}
	return domain.ToolResult{CallID: prepared.Call.ID, Status: domain.ToolStatusSuccess, Content: []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}}, StartedAt: started, FinishedAt: time.Now()}
}

// boundedRecords budgets encoded JSON, not raw characters: escaping can expand
// text several-fold. Truncation never splits an entry or returns malformed JSON.
func boundedRecords(result SearchResult) (SearchResult, error) {
	used := 64 // envelope and truncation flag
	for i, record := range result.Records {
		raw, err := json.Marshal(record)
		if err != nil {
			return SearchResult{}, err
		}
		if used+len(raw)+1 > maxToolBytes {
			result.Records = result.Records[:i]
			result.Truncated = true
			break
		}
		used += len(raw) + 1
	}
	return result, nil
}

func memoryToolError(id domain.ToolCallID, started time.Time, err error) domain.ToolResult {
	code, message, retryable := "internal", err.Error(), true
	if agentErr, ok := errors.AsType[*domain.AgentError](err); ok {
		code, message = string(agentErr.Code), agentErr.Message
		retryable = agentErr.Code != domain.ErrInvalidInput && agentErr.Code != domain.ErrConflict
	}
	return domain.ToolResult{CallID: id, Status: domain.ToolStatusError, Error: &domain.ToolError{Code: code, Message: message, Retryable: retryable}, StartedAt: started, FinishedAt: time.Now()}
}
