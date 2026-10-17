// Copyright (c) 2026 The Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func newTestMemoryTool(t *testing.T, s *Store) *MemoryTool {
	t.Helper()
	tool, err := NewMemoryTool(s)
	if err != nil {
		t.Fatal(err)
	}
	return tool
}

func prepareMemory(t *testing.T, tool *MemoryTool, args string) domain.PreparedCall {
	t.Helper()
	prepared, err := tool.Prepare(t.Context(), domain.ToolCall{ID: domain.NewToolCallID(), Name: ToolMemory, Arguments: json.RawMessage(args)})
	if err != nil {
		t.Fatalf("Prepare(%s): %v", args, err)
	}
	return prepared
}

func memoryJSON(t *testing.T, args any) string {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func decodeMemoryResult(t *testing.T, result domain.ToolResult, out any) {
	t.Helper()
	if result.Status != domain.ToolStatusSuccess || len(result.Content) != 1 || result.Content[0].Kind != domain.PartText {
		t.Fatalf("unexpected result: %+v", result)
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), out); err != nil {
		t.Fatalf("result JSON: %v", err)
	}
}

func requireMemoryToolError(t *testing.T, result domain.ToolResult, code domain.ErrorCode) {
	t.Helper()
	if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Code != string(code) {
		t.Fatalf("result=%+v; want error %s", result, code)
	}
	if (code == domain.ErrInvalidInput || code == domain.ErrConflict) && result.Error.Retryable {
		t.Fatal("invalid input/conflict must not be reported as retryable success")
	}
}

func TestMemoryToolDefinition(t *testing.T) {
	tool := newTestMemoryTool(t, newTestStore(t))
	def := tool.Definition()
	if def.Name != ToolMemory || def.Risk() != domain.R0 || !tool.ConcurrentSafe() {
		t.Fatalf("definition=%+v concurrent=%v", def, tool.ConcurrentSafe())
	}
	var schema struct {
		AdditionalProperties bool                       `json:"additionalProperties"`
		Properties           map[string]json.RawMessage `json:"properties"`
	}
	if err := json.Unmarshal(def.InputSchema, &schema); err != nil {
		t.Fatal(err)
	}
	if schema.AdditionalProperties || schema.Properties["id"] == nil || schema.Properties["revision"] == nil || schema.Properties["scope"] == nil {
		t.Fatalf("schema missing active-record constraints: %s", def.InputSchema)
	}
	for _, store := range []*Store{nil, NewStore(nil)} {
		if _, err := NewMemoryTool(store); err == nil {
			t.Fatal("missing repository accepted")
		}
	}
}

func TestMemoryToolPrepareCanonicalRiskAndIdempotency(t *testing.T) {
	tool := newTestMemoryTool(t, newTestStore(t).ForWorkspace("/project"))
	for _, tc := range []struct {
		args string
		risk domain.RiskLevel
	}{
		{`{"action":"list"}`, domain.R1},
		{`{"action":"search","query":"  Go bazel  "}`, domain.R1},
		{`{"action":"read","id":"active"}`, domain.R1},
		{`{"action":"read","id":"active","byte_offset":"3"}`, domain.R1},
		{`{"action":"remember","content":"  fact  "}`, domain.R2},
		{`{"action":"remember","scope":"workspace","id":"active","revision":"2","content":"update"}`, domain.R2},
		{`{"action":"forget","id":"active","revision":1}`, domain.R2},
		{`{"action":"add_note","filename":"2026-08-02T12-00-00-prefer-go.md","note":"fact"}`, domain.R2},
		{`{"action":"add_note","filename":"prefer-go.md","note":"fact"}`, domain.R2},
		{`{"action":"add_note","filename":"../not-a-storage-path","note":"fact"}`, domain.R2},
		{`{"action":"list","max_results":"2","scope":null}`, domain.R1},
	} {
		t.Run(tc.args, func(t *testing.T) {
			first := prepareMemory(t, tool, tc.args)
			second, err := tool.Prepare(t.Context(), first.Call)
			if err != nil || first.Risk != tc.risk || second.Risk != first.Risk || string(first.Call.Arguments) != string(second.Call.Arguments) || first.ArgsHash != second.ArgsHash || first.ApprovalDesc != second.ApprovalDesc || first.Call.ID != second.Call.ID {
				t.Fatalf("canonical replay drift: first=%+v second=%+v err=%v", first, second, err)
			}
			if first.ArgsHash == "" || strings.Contains(string(first.Call.Arguments), "filename") || strings.Contains(string(first.Call.Arguments), `"note"`) {
				t.Fatalf("legacy label retained in canonical mutation: %s", first.Call.Arguments)
			}
		})
	}
	legacy := prepareMemory(t, tool, `{"action":"add_note","filename":"2026-08-02T12-00-00-x.md","note":" fact "}`)
	modern := prepareMemory(t, tool, `{"action":"remember","content":"fact","scope":"global"}`)
	if string(legacy.Call.Arguments) != string(modern.Call.Arguments) || legacy.ArgsHash != modern.ArgsHash {
		t.Fatalf("alias not canonical: %s vs %s", legacy.Call.Arguments, modern.Call.Arguments)
	}
	changed := prepareMemory(t, tool, `{"action":"remember","content":"different","scope":"global"}`)
	workspace := prepareMemory(t, tool, `{"action":"remember","content":"fact","scope":"workspace"}`)
	update := prepareMemory(t, tool, `{"action":"remember","content":"fact","id":"active","revision":1}`)
	revision := prepareMemory(t, tool, `{"action":"remember","content":"fact","id":"active","revision":2}`)
	if changed.ArgsHash == modern.ArgsHash || workspace.ArgsHash == modern.ArgsHash || update.ArgsHash == revision.ArgsHash {
		t.Fatal("approval hash did not bind content, scope and revision")
	}
}

func TestMemoryToolPrepareValidation(t *testing.T) {
	tool := newTestMemoryTool(t, newTestStore(t).ForWorkspace("/project"))
	for name, args := range map[string]string{
		"invalid JSON":             `{invalid}`,
		"missing action":           `{}`,
		"unknown action":           `{"action":"delete"}`,
		"unknown field":            `{"action":"list","extra":true}`,
		"unknown null field":       `{"action":"list","extra":null}`,
		"old path field":           `{"action":"read","id":"active","path":"MEMORY.md"}`,
		"read ID required":         `{"action":"read"}`,
		"search empty":             `{"action":"search","query":"  "}`,
		"content empty":            `{"action":"remember","content":"  "}`,
		"aliases disagree":         `{"action":"remember","content":"one","note":"two"}`,
		"invalid scope":            `{"action":"remember","scope":"/project","content":"fact"}`,
		"negative revision":        `{"action":"remember","revision":-1,"content":"fact"}`,
		"update revision required": `{"action":"remember","id":"active","content":"fact"}`,
		"forget revision required": `{"action":"forget","id":"active"}`,
		"forget ID required":       `{"action":"forget","revision":1}`,
		"forget content":           `{"action":"forget","id":"active","revision":1,"content":"fact"}`,
		"negative byte offset":     `{"action":"read","id":"active","byte_offset":-1}`,
		"fractional byte offset":   `{"action":"read","id":"active","byte_offset":1.5}`,
		"negative offset":          `{"action":"read","id":"active","line_offset":-1}`,
		"negative lines":           `{"action":"read","id":"active","max_lines":-1}`,
		"too many lines":           `{"action":"read","id":"active","max_lines":501}`,
		"negative results":         `{"action":"list","max_results":-1}`,
		"too many results":         `{"action":"search","query":"x","max_results":201}`,
		"fractional revision":      `{"action":"forget","id":"active","revision":1.5}`,
		"query byte limit":         memoryJSON(t, memoryArgs{Action: ActionSearch, Query: strings.Repeat("界", MaxQueryBytes/3+1)}),
		"content byte limit":       memoryJSON(t, memoryArgs{Action: ActionRemember, Content: strings.Repeat("界", MaxContentBytes/3+1)}),
	} {
		t.Run(name, func(t *testing.T) {
			_, err := tool.Prepare(t.Context(), domain.ToolCall{ID: domain.NewToolCallID(), Name: ToolMemory, Arguments: json.RawMessage(args)})
			if err == nil {
				t.Fatal("invalid arguments accepted")
			}
			agentErr, ok := errors.AsType[*domain.AgentError](err)
			if !ok || agentErr.Code != domain.ErrInvalidInput {
				t.Fatalf("unexpected validation error: %v", err)
			}
		})
	}
	unbound := newTestMemoryTool(t, newTestStore(t))
	if _, err := unbound.Prepare(t.Context(), domain.ToolCall{ID: domain.NewToolCallID(), Arguments: json.RawMessage(`{"action":"remember","scope":"workspace","content":"fact"}`)}); err == nil {
		t.Fatal("workspace mutation accepted without host binding")
	}
	boundary := strings.Repeat("界", MaxContentBytes/3) + "x"
	prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRemember, Content: boundary}))
	prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionSearch, Query: strings.Repeat("x", MaxQueryBytes)}))
	prepareMemory(t, tool, `{"action":"read","id":"active","max_lines":500}`)
	prepareMemory(t, tool, `{"action":"list","max_results":200}`)
}

func TestMemoryToolRememberReplaySourceAndSameLabel(t *testing.T) {
	s := newTestStore(t).ForWorkspace("/project/a")
	tool := newTestMemoryTool(t, s)
	origin := domain.ToolContext{SessionID: domain.NewSessionID(), SourceVersion: 42}
	ctx := domain.WithToolContext(t.Context(), origin)
	first := prepareMemory(t, tool, `{"action":"add_note","filename":"2026-08-02T12-00-00-same.md","note":"project fact","scope":"workspace"}`)
	type mutationResult struct {
		Record             Record `json:"record"`
		Status             string `json:"status"`
		VisibleImmediately bool   `json:"visible_immediately"`
	}
	var one, replay, two mutationResult
	decodeMemoryResult(t, tool.Execute(ctx, first), &one)
	decodeMemoryResult(t, tool.Execute(ctx, first), &replay)
	if one.Record != replay.Record || one.Record.ID == "" || one.Status != ActionRemember || !one.VisibleImmediately {
		t.Fatalf("write/replay=%+v/%+v", one, replay)
	}
	if one.Record.Scope != s.Workspace() || one.Record.SessionID != origin.SessionID || one.Record.SourceVersion != origin.SourceVersion {
		t.Fatalf("source/binding not propagated: %+v", one.Record)
	}
	second := prepareMemory(t, tool, `{"action":"add_note","filename":"2026-08-02T12-00-00-same.md","note":"second project fact","scope":"workspace"}`)
	decodeMemoryResult(t, tool.Execute(ctx, second), &two)
	if two.Record.ID == one.Record.ID {
		t.Fatal("same compatibility label collided across distinct call IDs")
	}
	repo := s.repo.(*testRepo)
	if repo.seenCtx != ctx || repo.mutations[0].RequestID != first.Call.ID.String() || repo.mutations[0].SessionID != origin.SessionID || repo.mutations[0].SourceVersion != origin.SourceVersion {
		t.Fatalf("mutation lost execution context: %+v", repo.mutations)
	}
	drift := first
	drift.Call.Arguments = json.RawMessage(`{"action":"remember","scope":"workspace","content":"changed"}`)
	requireMemoryToolError(t, tool.Execute(ctx, drift), domain.ErrConflict)
	records, err := NewStore(repo).ForWorkspace(s.Workspace()).Records(ctx)
	if err != nil || len(records) != 2 {
		t.Fatalf("active records=%+v err=%v", records, err)
	}
	prompt, err := NewPromptProvider(NewStore(repo).ForWorkspace(s.Workspace())).MemoryPrompt(ctx)
	if err != nil || !strings.Contains(prompt, one.Record.Content) || !strings.Contains(prompt, two.Record.Content) {
		t.Fatalf("cross-Store prompt not immediately visible: %q err=%v", prompt, err)
	}
	other, err := NewPromptProvider(NewStore(repo).ForWorkspace("/project/b")).MemoryPrompt(ctx)
	if err != nil || strings.Contains(other, "project fact") {
		t.Fatalf("workspace fact leaked: %q err=%v", other, err)
	}
}

func TestMemoryToolConcurrentCallIDReplay(t *testing.T) {
	s := newTestStore(t)
	tool := newTestMemoryTool(t, s)
	prepared := prepareMemory(t, tool, `{"action":"remember","content":"one fact"}`)
	results := make(chan domain.ToolResult, 8)
	var wg sync.WaitGroup
	for range 8 {
		wg.Go(func() { results <- tool.Execute(t.Context(), prepared) })
	}
	wg.Wait()
	close(results)
	for result := range results {
		var payload struct {
			Record Record `json:"record"`
		}
		decodeMemoryResult(t, result, &payload)
		if payload.Record.Revision != 1 {
			t.Fatalf("replay mutated revision: %+v", payload.Record)
		}
	}
	records, err := s.Records(t.Context())
	if err != nil || len(records) != 1 {
		t.Fatalf("duplicate records after replay=%+v err=%v", records, err)
	}
}

func TestMemoryToolReportsAutomaticEvidenceRevocation(t *testing.T) {
	for _, scope := range []string{"global", "workspace"} {
		for _, action := range []string{ActionRemember, ActionForget} {
			t.Run(scope+"/"+action, func(t *testing.T) {
				s := newTestStore(t).ForWorkspace("/project")
				tool := newTestMemoryTool(t, s)
				var created struct {
					Record  Record `json:"record"`
					Revoked string `json:"automatic_evidence_revoked"`
				}
				decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRemember, Scope: scope, Content: "old fact"}))), &created)
				if created.Revoked != "" {
					t.Fatalf("new record reported evidence revocation: %+v", created)
				}
				args := memoryArgs{Action: action, Scope: scope, ID: created.Record.ID, Revision: created.Record.Revision}
				if action == ActionRemember {
					args.Content = "replacement"
				}
				var result struct {
					Revoked string `json:"automatic_evidence_revoked"`
				}
				decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, memoryJSON(t, args))), &result)
				want := "workspace"
				if scope == "global" {
					want = "all_workspaces"
				}
				if result.Revoked != want {
					t.Fatalf("revocation=%q want %q", result.Revoked, want)
				}
			})
		}
	}
}

func TestMemoryToolUpdateForgetAndInvalidForget(t *testing.T) {
	s := newTestStore(t).ForWorkspace("/project")
	tool := newTestMemoryTool(t, s)
	record := mustMutateMemory(t, s, Mutation{RequestID: "seed", Scope: s.Workspace(), Content: "old fact"})
	args := memoryArgs{Action: ActionRemember, Scope: "workspace", ID: record.ID, Revision: record.Revision, Content: "corrected fact"}
	var payload struct {
		Record Record `json:"record"`
	}
	decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, memoryJSON(t, args))), &payload)
	if payload.Record.Revision != 2 || payload.Record.Content != args.Content {
		t.Fatalf("update=%+v", payload)
	}
	for _, tc := range []struct {
		id, scope string
		revision  int64
		code      domain.ErrorCode
	}{
		{record.ID, "workspace", 1, domain.ErrConflict},
		{record.ID, "global", 2, domain.ErrInvalidInput},
		{"missing", "workspace", 1, domain.ErrInvalidInput},
	} {
		prepared := prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionForget, ID: tc.id, Scope: tc.scope, Revision: tc.revision}))
		requireMemoryToolError(t, tool.Execute(t.Context(), prepared), tc.code)
	}
	forget := prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionForget, Scope: "workspace", ID: record.ID, Revision: 2}))
	decodeMemoryResult(t, tool.Execute(t.Context(), forget), &payload)
	decodeMemoryResult(t, tool.Execute(t.Context(), forget), &payload)
	read := prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRead, ID: record.ID}))
	result := tool.Execute(t.Context(), read)
	requireMemoryToolError(t, result, domain.ErrInvalidInput)
	if !strings.Contains(result.Error.Message, "action=list") || !strings.Contains(result.Error.Message, "action=search") {
		t.Fatalf("missing ID guidance=%+v", result.Error)
	}
	for _, args := range []string{`{"action":"forget"}`, `{"action":"forget","id":"missing"}`} {
		requireMemoryToolError(t, tool.Execute(t.Context(), domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID(), Arguments: json.RawMessage(args)}}), domain.ErrInvalidInput)
	}
}

func TestMemoryToolListSearchLimitsAndPreviews(t *testing.T) {
	s := newTestStore(t).ForWorkspace("/project")
	repo := s.repo.(*testRepo)
	var records []Record
	for i := range 65 {
		records = append(records, Record{ID: fmt.Sprintf("id-%03d", i), Kind: Automatic, Revision: 1, Content: "keyword " + strings.Repeat("界", 120), Rollout: "private rollout detail"})
	}
	repo.fixture(Snapshot{Version: 65, Records: records})
	repo.fixture(Snapshot{Scope: "/other", Version: 1, Records: []Record{{ID: "hidden", Kind: Explicit, Content: "keyword hidden"}}})
	tool := newTestMemoryTool(t, s)
	for _, tc := range []struct {
		args           string
		count, preview int
		truncated      bool
	}{
		{`{"action":"list"}`, DefaultListMaxResults, 240, true},
		{`{"action":"list","max_results":1}`, 1, 240, true},
		{`{"action":"list","max_results":65}`, 65, 240, false},
		{`{"action":"search","query":"KEYWORD"}`, DefaultSearchMaxResults, 512, true},
		{`{"action":"search","query":"keyword","max_results":2}`, 2, 512, true},
		{`{"action":"search","query":"rollout detail","max_results":65}`, 65, 512, false},
		{`{"action":"search","query":"absent"}`, 0, 512, false},
	} {
		t.Run(tc.args, func(t *testing.T) {
			var result SearchResult
			decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, tc.args)), &result)
			if len(result.Records) != tc.count || result.Truncated != tc.truncated {
				t.Fatalf("result count=%d truncated=%v", len(result.Records), result.Truncated)
			}
			for _, record := range result.Records {
				if len(record.Content) > tc.preview || !utf8.ValidString(record.Content) || record.Rollout != "" || record.ID == "hidden" || record.Revision != 1 {
					t.Fatalf("invalid preview=%+v", record)
				}
			}
		})
	}
	original, err := s.Read(t.Context(), "id-000")
	if err != nil || original.Content != records[0].Content || original.Rollout != records[0].Rollout {
		t.Fatalf("preview changed repository evidence: %+v err=%v", original, err)
	}
}

// Regression contract: max_results alone is not an output-byte budget.
// Budget the encoded JSON, including control-character and quote escaping.
func TestMemoryToolListSearchOutputByteBudget(t *testing.T) {
	for _, tc := range []struct{ name, content string }{
		{"UTF8", "keyword " + strings.Repeat("界", 1300)},
		{"JSON escaping", "keyword " + strings.Repeat("\u0001\"\\", 1300)},
	} {
		for _, action := range []string{ActionList, ActionSearch} {
			t.Run(tc.name+"/"+action, func(t *testing.T) {
				s := newTestStore(t)
				var records []Record
				for i := range 201 {
					records = append(records, Record{ID: fmt.Sprintf("id-%03d", i), Kind: Explicit, Revision: 1, Content: tc.content})
				}
				s.repo.(*testRepo).fixture(Snapshot{Version: 201, Records: records})
				tool := newTestMemoryTool(t, s)
				args := memoryArgs{Action: action, MaxResults: 200}
				if action == ActionSearch {
					args.Query = "keyword"
				}
				result := tool.Execute(t.Context(), prepareMemory(t, tool, memoryJSON(t, args)))
				var payload SearchResult
				decodeMemoryResult(t, result, &payload)
				if !payload.Truncated || len(payload.Records) > 200 {
					t.Fatalf("overflow not signalled: count=%d truncated=%v", len(payload.Records), payload.Truncated)
				}
				if got := len(result.Content[0].Text); got > maxToolBytes {
					t.Errorf("%s JSON is %d bytes, exceeds %d-byte tool budget; bound aggregate previews, not just max_results", action, got, maxToolBytes)
				}
			})
		}
	}
}

type memoryReadPayload struct {
	Record         Record `json:"record"`
	Content        string `json:"content"`
	TotalLines     int    `json:"total_lines"`
	Truncated      bool   `json:"truncated"`
	NextLineOffset int    `json:"next_line_offset"`
	NextByteOffset int    `json:"next_byte_offset"`
	TotalBytes     int    `json:"total_bytes"`
}

func TestMemoryToolReadAutomaticRolloutPagination(t *testing.T) {
	s := newTestStore(t)
	var lines []string
	for i := range 250 {
		lines = append(lines, fmt.Sprintf("line%d", i+1))
	}
	s.repo.(*testRepo).fixture(Snapshot{Version: 1, Records: []Record{{ID: "automatic", Kind: Automatic, Revision: 3, Content: "recap", Rollout: strings.Join(lines, "\n")}}})
	tool := newTestMemoryTool(t, s)
	var parts []string
	offset := 1
	for page := range 3 {
		args := memoryArgs{Action: ActionRead, ID: "automatic"}
		if page > 0 {
			args.LineOffset = offset
		}
		var payload memoryReadPayload
		decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, memoryJSON(t, args))), &payload)
		if payload.TotalLines != 252 || payload.Record.Revision != 3 || payload.Record.Kind != Automatic || payload.Record.Content != "" || payload.Record.Rollout != "" {
			t.Fatalf("read metadata=%+v", payload)
		}
		wantLines := min(DefaultReadMaxLines, 253-offset)
		if len(strings.Split(payload.Content, "\n")) != wantLines || payload.NextLineOffset != offset+wantLines || payload.Truncated != (page < 2) {
			t.Fatalf("page %d: %+v", page, payload)
		}
		parts = append(parts, payload.Content)
		offset = payload.NextLineOffset
	}
	if strings.Join(parts, "\n") != "recap\n\n"+strings.Join(lines, "\n") {
		t.Fatal("rollout pagination skipped or duplicated lines")
	}
	for _, tc := range []struct {
		offset, max int
		want        string
		next        int
		truncated   bool
	}{
		{2, 2, "\nline1", 4, true},
		{500, 1, "", 253, false},
	} {
		var payload memoryReadPayload
		decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRead, ID: "automatic", LineOffset: tc.offset, MaxLines: tc.max}))), &payload)
		if payload.Content != tc.want || payload.NextLineOffset != tc.next || payload.Truncated != tc.truncated {
			t.Fatalf("offset read=%+v", payload)
		}
	}
}

func TestMemoryToolReadByteBudgetUTF8(t *testing.T) {
	s := newTestStore(t)
	s.repo.(*testRepo).fixture(Snapshot{Version: 1, Records: []Record{{ID: "big", Kind: Automatic, Revision: 1, Rollout: strings.Repeat("界", maxToolBytes)}}})
	tool := newTestMemoryTool(t, s)
	result := tool.Execute(t.Context(), prepareMemory(t, tool, `{"action":"read","id":"big"}`))
	var payload memoryReadPayload
	decodeMemoryResult(t, result, &payload)
	if len(result.Content[0].Text) > maxToolBytes || len(payload.Content) > maxToolBytes/2 || !utf8.ValidString(payload.Content) || !payload.Truncated {
		t.Fatalf("read byte budget: JSON=%d content=%d truncated=%v", len(result.Content[0].Text), len(payload.Content), payload.Truncated)
	}
}

func TestMemoryToolReadByteContinuation(t *testing.T) {
	for _, tc := range []struct{ name, text string }{
		{"long UTF8 single line", strings.Repeat("界𐐀", 10000)},
		{"JSON escaped single line", strings.Repeat("\u0001\"\\界", 10000)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := newTestStore(t)
			s.repo.(*testRepo).fixture(Snapshot{Version: 1, Records: []Record{{ID: "long", Kind: Automatic, Revision: 1, Content: tc.text}}})
			tool := newTestMemoryTool(t, s)
			var joined strings.Builder
			offset, pages := 0, 0
			for offset < len(tc.text) {
				prepared := prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRead, ID: "long", ByteOffset: offset}))
				fresh, err := tool.Prepare(t.Context(), prepared.Call)
				if err != nil || string(fresh.Call.Arguments) != string(prepared.Call.Arguments) || fresh.ArgsHash != prepared.ArgsHash || fresh.Risk != domain.R1 {
					t.Fatalf("byte_offset canonical drift at %d: %+v err=%v", offset, fresh, err)
				}
				result := tool.Execute(t.Context(), fresh)
				var payload memoryReadPayload
				decodeMemoryResult(t, result, &payload)
				if len(result.Content[0].Text) > maxToolBytes || !utf8.ValidString(payload.Content) || payload.Content == "" {
					t.Fatalf("invalid page at %d: JSON bytes=%d content bytes=%d", offset, len(result.Content[0].Text), len(payload.Content))
				}
				if payload.TotalLines != 1 || payload.TotalBytes != len(tc.text) || payload.NextByteOffset != offset+len(payload.Content) || payload.Truncated != (payload.NextByteOffset < len(tc.text)) {
					t.Fatalf("incorrect continuation metadata at %d: next=%d total=%d truncated=%v", offset, payload.NextByteOffset, payload.TotalBytes, payload.Truncated)
				}
				var fields map[string]json.RawMessage
				decodeMemoryResult(t, result, &fields)
				if _, present := fields["next_line_offset"]; present {
					t.Fatal("byte-limited/byte-offset read returned a line cursor that could skip unread text")
				}
				if payload.Content != tc.text[offset:payload.NextByteOffset] {
					t.Fatalf("page skipped or duplicated bytes at %d", offset)
				}
				joined.WriteString(payload.Content)
				offset = payload.NextByteOffset
				pages++
			}
			if pages < 2 || joined.String() != tc.text {
				t.Fatalf("continuation failed to reconstruct original text: pages=%d bytes=%d", pages, joined.Len())
			}
		})
	}
}

func TestMemoryToolReadRejectsMiddleUTF8ByteOffset(t *testing.T) {
	s := newTestStore(t)
	s.repo.(*testRepo).fixture(Snapshot{Version: 1, Records: []Record{{ID: "utf8", Kind: Automatic, Revision: 1, Content: "界𐐀tail"}}})
	tool := newTestMemoryTool(t, s)
	for _, offset := range []int{1, 2, 4, 5, 6} {
		prepared := prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRead, ID: "utf8", ByteOffset: offset}))
		result := tool.Execute(t.Context(), prepared)
		requireMemoryToolError(t, result, domain.ErrInvalidInput)
		if !strings.Contains(result.Error.Message, "UTF-8 boundary") {
			t.Fatalf("missing boundary guidance: %+v", result.Error)
		}
	}
	var payload memoryReadPayload
	decodeMemoryResult(t, tool.Execute(t.Context(), prepareMemory(t, tool, `{"action":"read","id":"utf8","byte_offset":3}`)), &payload)
	if payload.Content != "𐐀tail" || payload.Truncated || payload.NextByteOffset != len("界𐐀tail") {
		t.Fatalf("valid boundary rejected: %+v", payload)
	}
}

// Fail the consumer methods directly, independently of snapshot-loading behavior.
type failingMemoryConsumerRepo struct {
	*testRepo
	failure error
}

func (r *failingMemoryConsumerRepo) QueryMemory(context.Context, string, string, int) (SearchResult, error) {
	return SearchResult{}, r.failure
}

func (r *failingMemoryConsumerRepo) ReadMemory(context.Context, string, string) (Record, error) {
	return Record{}, r.failure
}

func TestMemoryToolReadActionsRepositoryErrors(t *testing.T) {
	failure := errors.New("consumer query unavailable")
	repo := &failingMemoryConsumerRepo{testRepo: newTestStore(t).repo.(*testRepo), failure: failure}
	tool := newTestMemoryTool(t, NewStore(repo))
	for _, args := range []string{`{"action":"list"}`, `{"action":"search","query":"fact"}`, `{"action":"read","id":"active"}`} {
		t.Run(args, func(t *testing.T) {
			prepared := prepareMemory(t, tool, args)
			result := tool.Execute(t.Context(), prepared)
			if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Message != failure.Error() || result.CallID != prepared.Call.ID || len(result.Content) != 0 {
				t.Fatalf("repository error returned a successful/partial payload: %+v", result)
			}
		})
	}
}

func TestMemoryToolSecretRedactionAndErrors(t *testing.T) {
	s := newTestStore(t)
	tool := newTestMemoryTool(t, s)
	secret := "sk-abcdefghijklmnopqrstuv"
	prepared := prepareMemory(t, tool, memoryJSON(t, memoryArgs{Action: ActionRemember, Content: "token " + secret}))
	if strings.Contains(string(prepared.Call.Arguments), secret) || strings.Contains(prepared.ApprovalDesc, secret) {
		t.Fatal("secret leaked through canonical arguments or approval description")
	}
	var payload struct {
		Record Record `json:"record"`
	}
	decodeMemoryResult(t, tool.Execute(t.Context(), prepared), &payload)
	if payload.Record.Content != "token "+redactPlaceholder {
		t.Fatalf("secret persisted: %+v", payload.Record)
	}
	failure := errors.New("repository offline")
	s.repo.(*testRepo).err = failure
	for _, args := range []string{`{"action":"list"}`, `{"action":"search","query":"token"}`, `{"action":"read","id":"missing"}`, `{"action":"remember","content":"fact"}`, `{"action":"forget","id":"missing","revision":1}`} {
		result := tool.Execute(t.Context(), prepareMemory(t, tool, args))
		if result.Status != domain.ToolStatusError || result.Error == nil || !strings.Contains(result.Error.Message, failure.Error()) {
			t.Fatalf("repository failure swallowed: %+v", result)
		}
	}
	s.repo.(*testRepo).err = nil
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	result := tool.Execute(ctx, prepareMemory(t, tool, `{"action":"remember","content":"cancelled fact"}`))
	if result.Status != domain.ToolStatusError || result.Error == nil || !strings.Contains(result.Error.Message, "context canceled") {
		t.Fatalf("cancellation swallowed: %+v", result)
	}
}
