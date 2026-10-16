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
	"io"
	"slices"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

func TestExtractionTwoFieldContract(t *testing.T) {
	properties := extractionResponseSchema["properties"].(map[string]any)
	if len(properties) != 2 || properties["raw_memory"] == nil || properties["rollout_summary"] == nil ||
		!slices.Equal(extractionResponseSchema["required"].([]string), []string{"rollout_summary", "raw_memory"}) ||
		extractionResponseSchema["additionalProperties"] != false {
		t.Fatalf("unexpected extraction schema: %+v", extractionResponseSchema)
	}
	var result ExtractionResult
	// Legacy providers and replay fixtures may still send unknown fields.
	if err := json.Unmarshal([]byte(`{"raw_memory":"fact","rollout_summary":"summary","rollout_slug":"legacy"}`), &result); err != nil {
		t.Fatal(err)
	}
	encoded, err := json.Marshal(result)
	if err != nil || string(encoded) != `{"rollout_summary":"summary","raw_memory":"fact"}` {
		t.Fatalf("output=%s err=%v", encoded, err)
	}
	for _, prompt := range []string{stageOneSystemPrompt, buildStageOneInput("session", "/workspace", "evidence")} {
		for _, obsolete := range []string{"rollout_slug", "MEMORY.md", "raw_memories.md", "rollout_summaries"} {
			if strings.Contains(prompt, obsolete) {
				t.Fatalf("obsolete storage instruction %q", obsolete)
			}
		}
	}
}

func TestCapTranscript(t *testing.T) {
	if got := capTranscript("short", 100); got != "short" {
		t.Fatalf("short transcript changed: %q", got)
	}
	text := "begin\n" + strings.Repeat("你好世界\n", 5000) + "I prefer table-driven tests\n" + strings.Repeat("routine log\n", 5000) + "end"
	got := capTranscript(text, 1000)
	if len(got) > 1000 || !utf8.ValidString(got) || !strings.Contains(got, "transcript truncated") ||
		!strings.Contains(got, "I prefer table-driven tests") || !strings.HasSuffix(got, "end") {
		t.Fatalf("invalid bounded transcript: bytes=%d, text=%q", len(got), got)
	}
	for limit := range 50 {
		got := capTranscript(text, limit)
		if len(got) > limit || !utf8.ValidString(got) {
			t.Fatalf("limit=%d, bytes=%d, invalid UTF-8=%v", limit, len(got), !utf8.ValidString(got))
		}
	}
}

func TestCapTranscriptRetainsPreferenceInLongSingleLine(t *testing.T) {
	preference := "I prefer explicit errors over silent fallback"
	text := strings.Repeat("x", 100000) + preference + strings.Repeat("z", 100000)
	got := capTranscript(text, 4096)
	if len(got) > 4096 || !strings.Contains(got, preference) {
		t.Fatal("preference in omitted single-line paste was lost")
	}
}

func TestCapTranscriptRetainsPreferenceAfterCaseFoldLengthChangingPrefix(t *testing.T) {
	for _, prefix := range []string{"İ", "ẞ", "İẞ"} {
		t.Run(prefix, func(t *testing.T) {
			preference := "I PREFER explicit errors over silent fallback"
			text := strings.Repeat(prefix, 50000) + preference + strings.Repeat("z", 100000)
			got := capTranscript(text, 4096)
			if len(got) > 4096 || !utf8.ValidString(got) || !strings.Contains(got, preference) {
				t.Fatalf("preference lost after case-fold byte length change: bytes=%d text=%q", len(got), got)
			}
		})
	}
}

func TestCapTranscriptRetainsEarliestCueInOriginalLine(t *testing.T) {
	preference := "Remember explicit errors instead of silent fallback"
	text := strings.Repeat("x", 100000) + preference + strings.Repeat("z", 100000) + "prefer silent fallback" + strings.Repeat("x", 100000)
	got := capTranscript(text, 4096)
	if len(got) > 4096 || !utf8.ValidString(got) || !strings.Contains(got, preference) {
		t.Fatal("cue priority selected a later cue instead of the earliest match")
	}
}

func TestExtractJSON(t *testing.T) {
	for _, input := range []string{
		"```json\n{\"key\": \"value\"}\n```",
		"```\n{\"key\": \"value\"}\n```",
		`Here is the result: {"key": "value"} and some text`,
	} {
		if got := extractJSON(input); got != `{"key": "value"}` {
			t.Fatalf("extractJSON = %q", got)
		}
	}
}

func evidenceMessage(role domain.Role, text string) domain.Message {
	return domain.Message{ID: domain.NewMessageID(), Role: role, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: text}}}
}

func TestSerializeMessagesPreservesLongUserPreferences(t *testing.T) {
	preference := "I prefer explicit errors, never silently discard them."
	message := evidenceMessage(domain.RoleUser, strings.Repeat("pasted logs\n", 1000)+preference)
	got := serializeMessages([]domain.Message{message})
	if !strings.Contains(got, preference) {
		t.Fatal("user preference beyond 4KB was discarded")
	}
	message.Parts[0].Text = strings.Repeat("日志\n", 50000) + preference + "\n" + strings.Repeat("log\n", 50000)
	got = serializeMessages([]domain.Message{message})
	if len(got) > maxTranscriptBytes || !utf8.ValidString(got) || !strings.Contains(got, preference) || !strings.Contains(got, "truncated") {
		t.Fatalf("oversized user evidence lost preference or budget: bytes=%d", len(got))
	}
}

func TestSerializeMessagesExcludesSynthetic(t *testing.T) {
	messages := []domain.Message{evidenceMessage(domain.RoleUser, "real preference")}
	for _, meta := range []map[string]string{
		{"compacted": "summary"},
		{"compacted": "archived"},
		{"kind": "budget_wrapup"},
		{"kind": "system_note"},
		{"kind": "budget_notice"},
		{"synthetic": "true"},
	} {
		msg := evidenceMessage(domain.RoleUser, "synthetic preference must disappear")
		msg.Metadata = meta
		messages = append(messages, msg)
	}
	got := serializeMessages(messages)
	if strings.Contains(got, "synthetic preference") || !strings.Contains(got, "real preference") {
		t.Fatalf("bad evidence selection: %s", got)
	}
}

func TestSerializeMessagesToolEvidenceIsBoundedAndRedacted(t *testing.T) {
	const secret = "sk-abcdefghijklmnop1234567890"
	callID := domain.NewToolCallID()
	messages := []domain.Message{{Role: domain.RoleAssistant, Parts: []domain.ContentPart{
		{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID, Name: "shell", Arguments: []byte(`{"command":"bazel test //...", "api_key":"` + secret + `", "paste":"` + strings.Repeat("x", 20000) + `"}`)}},
	}}, {Role: domain.RoleUser, Parts: []domain.ContentPart{
		{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
			CallID: callID, Status: domain.ToolStatusError,
			Error:   &domain.ToolError{Code: "exit", Message: "failed " + secret},
			Content: []domain.ContentPart{{Kind: domain.PartText, Text: "compiler failed " + secret + strings.Repeat("z", 20000)}},
		}},
	}}}
	got := serializeMessages(messages)
	for _, want := range []string{"Tool call evidence", "arguments=", "bazel test", "Tool result evidence", "status=error", "compiler failed", "truncated"} {
		if !strings.Contains(got, want) {
			t.Errorf("missing evidence %q", want)
		}
	}
	if strings.Contains(got, secret) || len(got) > 3*maxPartBytes+1000 {
		t.Fatalf("unbounded or unredacted evidence: bytes=%d", len(got))
	}
}

func TestExtractorPureResultRedactsEveryInputAndOutput(t *testing.T) {
	const secret = "sk-abcdefghijklmnop1234567890"
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: `{"raw_memory":"remember ` + secret + `", "rollout_summary":"summary ` + secret + `"}`, StopReason: domain.StopEndTurn})
	result, err := NewExtractor(model, "extract-model", true).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, secret)}, "/ws/"+secret)
	if err != nil || result == nil {
		t.Fatalf("extract: result=%+v err=%v", result, err)
	}
	if strings.Contains(result.RawMemory+result.RolloutSummary, secret) {
		t.Fatal("secret survived extraction result")
	}
	call := model.Calls()[0]
	if call.ModelName != "extract-model" || call.ResponseFormat == nil || !call.ResponseFormat.Strict {
		t.Fatalf("wrong model request: %+v", call)
	}
	for _, message := range call.Messages {
		if strings.Contains(strings.Join(message.TextParts(), ""), secret) {
			t.Fatal("secret survived input redaction")
		}
	}
	if input := buildStageOneInput(secret, secret, secret); strings.Contains(input, secret) {
		t.Fatal("session context leaked a secret")
	}
}

func TestExtractorFieldByteLimitsAfterRedaction(t *testing.T) {
	const secret = "sk-abcdefghijklmnop1234567890"
	for _, field := range []string{"raw_memory", "rollout_summary"} {
		for _, tc := range []struct {
			name    string
			text    string
			wantErr bool
		}{
			{"boundary", strings.Repeat("x", MaxExtractionFieldBytes), false},
			{"oversized", strings.Repeat("x", MaxExtractionFieldBytes+1), true},
			{"multibyte boundary", strings.Repeat("界", MaxExtractionFieldBytes/3) + "xx", false},
			{"multibyte oversized", strings.Repeat("界", MaxExtractionFieldBytes/3+1), true},
			{"oversized whitespace", strings.Repeat(" ", MaxExtractionFieldBytes+1), true},
			{"redaction shrinks below limit", strings.Repeat("x", MaxExtractionFieldBytes-len(redactPlaceholder)-1) + " " + secret, false},
			{"redaction expands past limit", strings.Repeat("x", MaxExtractionFieldBytes-len("password=abcdefgh")-1) + " password=abcdefgh", true},
		} {
			t.Run(field+"/"+tc.name, func(t *testing.T) {
				fields := map[string]string{"raw_memory": "", "rollout_summary": ""}
				fields[field] = tc.text
				output, err := json.Marshal(fields)
				if err != nil {
					t.Fatal(err)
				}
				model := fakes.NewFakeModel(fakes.ScriptEntry{Text: string(output), StopReason: domain.StopEndTurn})
				result, err := NewExtractor(model, "model", false).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "remember this")}, "")
				if tc.wantErr {
					if err == nil || result != nil || !strings.Contains(err.Error(), field) {
						t.Fatalf("oversized field accepted: result=%+v err=%v", result, err)
					}
					return
				}
				if err != nil || result == nil {
					t.Fatalf("valid field rejected: result=%+v err=%v", result, err)
				}
				got := result.RawMemory
				if field == "rollout_summary" {
					got = result.RolloutSummary
				}
				if got != RedactSecrets(tc.text) || len(got) > MaxExtractionFieldBytes || !utf8.ValidString(got) {
					t.Fatalf("wrong redacted field: bytes=%d", len(got))
				}
			})
		}
	}
}

func TestExtractorUTF8ValidationAndEscapedSurrogates(t *testing.T) {
	for _, field := range []string{"raw_memory", "rollout_summary"} {
		for _, tc := range []struct {
			name    string
			value   string
			want    string
			wantErr bool
		}{
			{"invalid raw UTF8", "\xff", "", true},
			{"escaped unpaired surrogate", `\ud800`, "\ufffd", false},
			{"escaped surrogate pair", `\ud83d\ude00`, "😀", false},
		} {
			t.Run(field+"/"+tc.name, func(t *testing.T) {
				model := fakes.NewFakeModel(fakes.ScriptEntry{Text: `{"` + field + `":"` + tc.value + `"}`, StopReason: domain.StopEndTurn})
				result, err := NewExtractor(model, "model", false).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "remember this")}, "")
				if tc.wantErr {
					if err == nil || result != nil {
						t.Fatalf("invalid raw UTF8 accepted: result=%+v err=%v", result, err)
					}
					return
				}
				if err != nil || result == nil {
					t.Fatalf("valid decoded JSON rejected: result=%+v err=%v", result, err)
				}
				got := result.RawMemory
				if field == "rollout_summary" {
					got = result.RolloutSummary
				}
				if got != tc.want || !utf8.ValidString(got) {
					t.Fatalf("decoded field=%q, want %q", got, tc.want)
				}
			})
		}
	}
}

func TestExtractorPreservesModelIdentifier(t *testing.T) {
	const modelName = "sk-abcdefghijklmnop1234567890"
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: `{"raw_memory":"fact","rollout_summary":""}`, StopReason: domain.StopEndTurn})
	_, err := NewExtractor(model, modelName, false).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "remember this")}, "")
	if err != nil || len(model.Calls()) != 1 || model.Calls()[0].ModelName != modelName {
		t.Fatalf("model identifier was altered: calls=%+v err=%v", model.Calls(), err)
	}
}

func TestExtractorOnlyNormalEndTurnSucceeds(t *testing.T) {
	for _, reason := range []domain.StopReason{domain.StopEndTurn, "", domain.StopUnknown, domain.StopMaxOutput, domain.StopCancelled, domain.StopProviderError, domain.StopToolUse, domain.StopContentFilter} {
		t.Run(string(reason), func(t *testing.T) {
			model := fakes.NewFakeModel(fakes.ScriptEntry{Text: `{"raw_memory":"useful", "rollout_summary":""}`, StopReason: reason})
			result, err := NewExtractor(model, "model", false).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "remember this")}, "")
			if reason == domain.StopEndTurn {
				if err != nil || result == nil {
					t.Fatalf("normal completion rejected: %v", err)
				}
			} else if err == nil || result != nil {
				t.Fatalf("nonterminal/failed completion accepted: %+v, %v", result, err)
			}
		})
	}
}

type extractionStreamModel struct{ stream *extractionTestStream }

func (m extractionStreamModel) Stream(context.Context, domain.ModelRequest) (domain.ModelStream, error) {
	return m.stream, nil
}

type extractionTestStream struct {
	events []domain.ModelEvent
	err    error
	closed bool
}

func (s *extractionTestStream) Recv() (domain.ModelEvent, error) {
	if len(s.events) == 0 {
		if s.err != nil {
			return domain.ModelEvent{}, s.err
		}
		return domain.ModelEvent{}, io.EOF
	}
	event := s.events[0]
	s.events = s.events[1:]
	return event, nil
}

func (s *extractionTestStream) Close() error { s.closed = true; return nil }

func TestExtractorRejectsTruncatedOrErroredStream(t *testing.T) {
	providerErr := errors.New("provider connection lost")
	for _, streamErr := range []error{nil, providerErr, context.Canceled} {
		stream := &extractionTestStream{events: []domain.ModelEvent{{Kind: domain.ModelEventTextDelta, TextDelta: `{"raw_memory":"valid JSON is not proof of completion"}`}}, err: streamErr}
		result, err := NewExtractor(extractionStreamModel{stream}, "model", false).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "remember this")}, "")
		if err == nil || result != nil || !stream.closed {
			t.Fatalf("incomplete stream accepted or left open: result=%+v err=%v closed=%v", result, err, stream.closed)
		}
		if streamErr != nil && !errors.Is(err, streamErr) {
			t.Fatalf("lost stream error: %v", err)
		}
	}
}

func TestExtractorRejectsStreamErrorEvent(t *testing.T) {
	stream := &extractionTestStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextDelta, TextDelta: `{"raw_memory":"useful"}`},
		{Kind: domain.ModelEventStreamError, Error: "provider failed"},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}}
	result, err := NewExtractor(extractionStreamModel{stream}, "model", false).ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "remember this")}, "")
	if err == nil || result != nil || !stream.closed {
		t.Fatalf("stream error event ignored: result=%+v err=%v", result, err)
	}
}

func TestExtractorNoOutputAndCancellation(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: `{"raw_memory":"", "rollout_summary":""}`, StopReason: domain.StopEndTurn})
	extractor := NewExtractor(model, "model", false)
	result, err := extractor.ExtractFromSession(t.Context(), domain.NewSessionID(), []domain.Message{evidenceMessage(domain.RoleUser, "hi")}, "")
	if result != nil || err != nil {
		t.Fatalf("empty result: %+v, %v", result, err)
	}
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	result, err = extractor.ExtractFromSession(ctx, domain.NewSessionID(), nil, "")
	if result != nil || !errors.Is(err, context.Canceled) || len(model.Calls()) != 1 {
		t.Fatalf("cancelled call misreported no-output: %+v, %v", result, err)
	}
}
