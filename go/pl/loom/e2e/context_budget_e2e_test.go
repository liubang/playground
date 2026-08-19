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
// Created: 2026/07/29

package e2e

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/model/openai"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// context_budget_e2e_test.go verifies the CONTEXT_DESIGN v2 mechanisms
// end to end: the real openai provider over HTTP/SSE against a scripted
// mock server, the real tool implementations (read_file/search) inside
// the real loop, and the real artifact store. Unlike the agent unit
// tests (fakes.FakeModel + fakes.FakeTool), these scenarios exercise the
// canonical-argument fingerprints of the production tools — the surface
// where the repeated-call detector's ArgsHash bug only showed up.

// mockToolCall is one tool call in a scripted response.
type mockToolCall struct {
	Name string
	Args string
}

// mockEntry is one scripted response of the mock OpenAI server.
type mockEntry struct {
	Text string // assistant text (finish "stop")
	// ToolName/ToolArgs script a single tool call; Tools scripts several
	// in one response (e.g. two parallel delegate_task calls).
	ToolName string
	ToolArgs string
	Tools    []mockToolCall
	UsageIn  int64
	UsageOut int64
	Delay    time.Duration
	// Fail makes the handler answer an HTTP error — provider-error
	// injection (e.g. a sub-agent's model call dying mid-delegation).
	// FailStatus selects the status (default 500); pick a non-retryable
	// 4xx when the scenario needs the run to fail fast — retryable 5xx
	// responses now go through the loop's bounded wait-and-retry, which
	// would consume the following scripted entries.
	Fail       bool
	FailStatus int
	// FailBody replaces the default error body (e.g. to inject provider
	// quota-exhaustion text riding a 429).
	FailBody string
	// Match routes this entry to the first unconsumed request whose body
	// contains the substring; empty matches anything. Entries are consumed
	// in declaration order among the matching ones — parallel sub-agents
	// make request order nondeterministic, content makes it stable.
	Match string
}

// mockOpenAI replays entries as chat-completions SSE streams and records
// every request body for assertions.
type mockOpenAI struct {
	t        *testing.T
	server   *httptest.Server
	mu       sync.Mutex
	entries  []mockEntry
	consumed []bool
	requests [][]byte
}

func newMockOpenAI(t *testing.T, entries []mockEntry) *mockOpenAI {
	t.Helper()
	m := &mockOpenAI{t: t, entries: entries, consumed: make([]bool, len(entries))}
	m.server = httptest.NewServer(http.HandlerFunc(m.handleChatCompletions))
	t.Cleanup(m.server.Close)
	return m
}

func (m *mockOpenAI) handleChatCompletions(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	m.mu.Lock()
	m.requests = append(m.requests, body)
	idx := -1
	for i := range m.entries {
		if m.consumed[i] {
			continue
		}
		if m.entries[i].Match == "" || strings.Contains(string(body), m.entries[i].Match) {
			idx = i
			m.consumed[i] = true
			break
		}
	}
	m.mu.Unlock()
	if idx < 0 {
		// A request without a scripted entry is a test-scripting bug, not a
		// transient provider error: answer a non-retryable 400 so the run
		// fails fast instead of burning the loop's retry budget on waits.
		http.Error(w, `{"error":{"message":"no matching mock entry"}}`, http.StatusBadRequest)
		return
	}
	entry := m.entries[idx]
	if entry.Fail {
		status := entry.FailStatus
		if status == 0 {
			status = http.StatusInternalServerError
		}
		body := entry.FailBody
		if body == "" {
			body = `{"error":{"message":"mock injected provider error"}}`
		}
		http.Error(w, body, status)
		return
	}
	if entry.Delay > 0 {
		time.Sleep(entry.Delay)
	}
	w.Header().Set("Content-Type", "text/event-stream")
	write := func(payload any) {
		data, err := json.Marshal(payload)
		if err != nil {
			m.t.Errorf("marshal SSE chunk: %v", err)
			return
		}
		fmt.Fprintf(w, "data: %s\n\n", data)
	}
	chunk := func(delta map[string]any, finish string) {
		write(map[string]any{
			"id": "chatcmpl-mock", "object": "chat.completion.chunk", "created": 0, "model": "mock-model",
			"choices": []map[string]any{{"index": 0, "delta": delta, "finish_reason": finish}},
		})
	}
	chunk(map[string]any{"role": "assistant", "content": ""}, "")
	if entry.Text != "" {
		chunk(map[string]any{"content": entry.Text}, "")
	}
	toolCalls := entry.Tools
	if entry.ToolName != "" {
		toolCalls = []mockToolCall{{Name: entry.ToolName, Args: entry.ToolArgs}}
	}
	if len(toolCalls) > 0 {
		calls := make([]map[string]any, 0, len(toolCalls))
		for i, tc := range toolCalls {
			calls = append(calls, map[string]any{
				"index": i, "id": fmt.Sprintf("call_mock_%d_%d", idx, i), "type": "function",
				"function": map[string]any{"name": tc.Name, "arguments": tc.Args},
			})
		}
		chunk(map[string]any{"tool_calls": calls}, "")
	}
	finish := "stop"
	if len(toolCalls) > 0 {
		finish = "tool_calls"
	}
	chunk(map[string]any{}, finish)
	write(map[string]any{
		"id": "chatcmpl-mock", "object": "chat.completion.chunk", "created": 0, "model": "mock-model",
		"choices": []any{},
		"usage":   map[string]any{"prompt_tokens": entry.UsageIn, "completion_tokens": entry.UsageOut},
	})
	fmt.Fprint(w, "data: [DONE]\n\n")
}

// requestCount reports how many requests reached the mock.
func (m *mockOpenAI) requestCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.requests)
}

// provider builds the real openai provider pointed at the mock.
func (m *mockOpenAI) provider(t *testing.T) domain.Model {
	t.Helper()
	model, err := openai.New(openai.Config{
		BaseURL: m.server.URL + "/v1",
		APIKey:  "mock",
	})
	if err != nil {
		t.Fatalf("openai.New() error = %v", err)
	}
	return model
}

// requestText flattens the messages of the n-th recorded request into
// searchable text.
func (m *mockOpenAI) requestText(t *testing.T, n int) string {
	t.Helper()
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.requests) <= n {
		t.Fatalf("mock received %d requests, want at least %d", len(m.requests), n+1)
	}
	var req struct {
		Messages []struct {
			Content json.RawMessage `json:"content"`
		} `json:"messages"`
	}
	if err := json.Unmarshal(m.requests[n], &req); err != nil {
		t.Fatalf("request %d is not valid JSON: %v", n, err)
	}
	var sb strings.Builder
	for _, msg := range req.Messages {
		var text string
		if err := json.Unmarshal(msg.Content, &text); err == nil {
			sb.WriteString(text + "\n")
			continue
		}
		var parts []struct {
			Text string `json:"text"`
		}
		if err := json.Unmarshal(msg.Content, &parts); err == nil {
			for _, p := range parts {
				sb.WriteString(p.Text + "\n")
			}
		}
	}
	return sb.String()
}

// realEnv wires the real tool implementations into a registry rooted at ws.
func realEnv(t *testing.T, ws string) (*agent.ToolRegistry, *artifact.Store) {
	t.Helper()
	validator, err := workspacepkg.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	registry := agent.NewToolRegistry()
	readTool, err := builtin.NewReadFileTool(validator, nil)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}
	searchTool, err := builtin.NewSearchTool(validator, nil)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	for _, tool := range []domain.Tool{readTool, searchTool} {
		if err := registry.Register(tool); err != nil {
			t.Fatalf("Register(%s) error = %v", tool.Definition().Name, err)
		}
	}
	artStore, err := artifact.Open(filepath.Join(ws, "artifacts"), 100<<20)
	if err != nil {
		t.Fatalf("artifact.Open() error = %v", err)
	}
	return registry, artStore
}

func newBudgetRun(t *testing.T, limits domain.Limits) *agent.Run {
	t.Helper()
	run := agent.NewRun(domain.NewSessionID(), limits, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "do the task"}},
		CreatedAt: time.Now().UTC(),
	})
	return run
}

func newRealLoop(run *agent.Run, model domain.Model, registry *agent.ToolRegistry, artStore domain.ArtifactStore, window agent.WindowModel) *agent.Loop {
	return &agent.Loop{
		Run: run, Model: model, ModelName: "mock-model",
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artStore,
		Window:    window,
	}
}

func writeFile(t *testing.T, ws, name, content string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(ws, name), []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func runEvents(run *agent.Run, eventType domain.EventType) []json.RawMessage {
	var out []json.RawMessage
	for _, evt := range run.PendingEvents() {
		if evt.Type == eventType {
			out = append(out, evt.Payload)
		}
	}
	return out
}

// E1: windowed compaction trigger (calibrated occupancy — metered
// footprint plus everything appended since), graduated notices at both
// levels, and a clean finish — against a 1000-token window where
// effective=950, notices=[570,712], trigger=760.
func TestE2EWindowedCompactionAndNotices(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "a.txt", "alpha\n")
	writeFile(t, ws, "b.txt", "beta\n")
	writeFile(t, ws, "c.txt", "gamma\n")
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "read_file", ToolArgs: `{"path":"a.txt"}`, UsageIn: 580, UsageOut: 30},
		{ToolName: "read_file", ToolArgs: `{"path":"b.txt"}`, UsageIn: 700, UsageOut: 30},
		{ToolName: "read_file", ToolArgs: `{"path":"c.txt"}`, UsageIn: 800, UsageOut: 30},
		{Text: "all done", UsageIn: 100, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(1000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	// Exactly one auto compaction: the calibrated occupancy (metered 800
	// plus the response/tool-result growth) crosses the 760 trigger after
	// the third call.
	compacted := runEvents(run, domain.EventContextCompacted)
	if len(compacted) != 1 {
		t.Fatalf("compactions = %d, want 1: %s", len(compacted), compacted)
	}
	var compaction struct {
		Trigger         string `json:"trigger"`
		OccupancyBefore int64  `json:"occupancy_before"`
	}
	if err := json.Unmarshal(compacted[0], &compaction); err != nil {
		t.Fatalf("compaction payload: %v", err)
	}
	if compaction.Trigger != "auto" || compaction.OccupancyBefore < 760 {
		t.Fatalf("compaction = %+v, want auto trigger at occupancy ≥ 760", compaction)
	}
	// Graduated notices fired at both occupancy levels (level 1 may refire
	// after the compaction re-arms it — that is by design).
	levels := map[int]bool{}
	for _, raw := range runEvents(run, domain.EventBudgetNotice) {
		var notice struct {
			Dimension string `json:"dimension"`
			Level     int    `json:"level"`
		}
		if err := json.Unmarshal(raw, &notice); err != nil {
			t.Fatalf("notice payload: %v", err)
		}
		if notice.Dimension == "occupancy" {
			levels[notice.Level] = true
		}
	}
	if !levels[1] || !levels[2] {
		t.Fatalf("occupancy notice levels = %v, want both 1 and 2", levels)
	}
	// The level-2 notice reached the model over the wire (injected in the
	// prepare phase before the third call).
	if text := mock.requestText(t, 2); !strings.Contains(text, "auto-compaction is imminent") {
		t.Fatalf("level-2 notice missing from the model's input")
	}
}

// E2: the incident regression — a 40k-occupancy run must see zero
// compactions on a 200k window (the hardcoded 32k target misfired here).
func TestE2ENoFalseCompactionBelowWindowTrigger(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "big.txt", strings.Repeat("x", 1000))
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "read_file", ToolArgs: `{"path":"big.txt"}`, UsageIn: 40_000, UsageOut: 30},
		{Text: "done", UsageIn: 41_000, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if n := len(runEvents(run, domain.EventContextCompacted)); n != 0 {
		t.Fatalf("compactions = %d, want 0 below the window trigger", n)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
}

// E3: soft landing — a session-token breach enters exactly one wrap-up
// turn and the run terminates with a conclusion, not mid-work.
func TestE2ESoftLandingTokens(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "a.txt", "data\n")
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "read_file", ToolArgs: `{"path":"a.txt"}`, UsageIn: 90, UsageOut: 20},
		{Text: "final summary with conclusions", UsageIn: 50, UsageOut: 20},
	})
	registry, artStore := realEnv(t, ws)
	limits := domain.DefaultLimits()
	limits.MaxTokens = 100 // first call accounts 110 ≥ 100 → wrap-up
	run := newBudgetRun(t, limits)
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want a clean soft-landing termination", err)
	}
	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("outcome = %s, want budget_exhausted", run.State.Outcome)
	}
	if n := len(runEvents(run, domain.EventBudgetWrapupStarted)); n != 1 {
		t.Fatalf("budget.wrapup_started events = %d, want 1", n)
	}
	if got := finalAssistantText(t, run); got != "final summary with conclusions" {
		t.Fatalf("final answer = %q, want the wrap-up summary", got)
	}
}

// E4: repeated identical calls against the REAL read_file — regression
// for the detector hashing the HMAC call signature (unique per call)
// instead of the canonical arguments. It must warn on the second repeat
// and terminate on the third.
func TestE2ERepeatedCallsTerminateWithRealTool(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "a.txt", "data\n")
	entries := make([]mockEntry, 3)
	for i := range entries {
		entries[i] = mockEntry{ToolName: "read_file", ToolArgs: `{"path":"a.txt"}`, UsageIn: 100, UsageOut: 10}
	}
	mock := newMockOpenAI(t, entries)
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))

	err := loop.Execute(context.Background())
	if err == nil || !strings.Contains(err.Error(), "identical arguments") {
		t.Fatalf("Execute() error = %v, want repeated-call termination", err)
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	// The warning was injected before termination, and every issued call
	// is paired with a result.
	if n := len(runEvents(run, domain.EventBudgetNotice)); n != 1 {
		t.Fatalf("runaway warning notices = %d, want 1", n)
	}
	if dangling := countUnresolved(run.Messages); dangling != 0 {
		t.Fatalf("transcript must stay paired, %d dangling calls", dangling)
	}
}

// E5: a legitimate 60-turn run completes — the deleted MaxTurns quota
// must not kill long work.
func TestE2ELongRunIsNotTurnCapped(t *testing.T) {
	ws := t.TempDir()
	entries := make([]mockEntry, 0, 61)
	for i := 0; i < 60; i++ {
		name := fmt.Sprintf("f%02d.txt", i)
		writeFile(t, ws, name, fmt.Sprintf("content %d\n", i))
		entries = append(entries, mockEntry{ToolName: "read_file", ToolArgs: fmt.Sprintf(`{"path":%q}`, name), UsageIn: 100, UsageOut: 10})
	}
	entries = append(entries, mockEntry{Text: "60 turns done", UsageIn: 100, UsageOut: 10})
	mock := newMockOpenAI(t, entries)
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want the 60-turn run to complete", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded || run.Usage.Turns != 61 {
		t.Fatalf("outcome = %s turns = %d, want succeeded in 61 turns", run.State.Outcome, run.Usage.Turns)
	}
}

// E6: oversized tool output is truncated head+tail at ingestion with a
// warning header — visible in the model's next request.
func TestE2EUnifiedOutputTruncation(t *testing.T) {
	ws := t.TempDir()
	var big strings.Builder
	for i := 0; i < 500; i++ {
		big.WriteString(strings.Repeat("0123456789", 60) + "\n")
	}
	writeFile(t, ws, "big.txt", big.String())
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "read_file", ToolArgs: `{"path":"big.txt","limit":500}`, UsageIn: 100, UsageOut: 10},
		{Text: "read it", UsageIn: 100, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	text := mock.requestText(t, 1)
	if !strings.Contains(text, "Warning: output truncated") || !strings.Contains(text, "[middle omitted]") {
		t.Fatalf("truncated tool result missing warning header/middle-omitted marker")
	}
}

// E7: prepare failures keep the event stream paired (degraded
// prepared/started events), and a malformed-arguments placeholder
// surfaces its embedded hint instead of the internal field name.
func TestE2EPrepareFailedPairingAndMalformedHint(t *testing.T) {
	ws := t.TempDir()
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "grep", ToolArgs: `{"pattern":""}`, UsageIn: 100, UsageOut: 10},
		{ToolName: "grep", ToolArgs: `{"pattern":`, UsageIn: 100, UsageOut: 10},
		{Text: "recovered", UsageIn: 100, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	prepared := runEvents(run, domain.EventToolCallPrepared)
	started := runEvents(run, domain.EventToolExecutionStarted)
	completed := runEvents(run, domain.EventToolExecutionCompleted)
	if len(prepared) != 2 || len(started) != 2 || len(completed) != 2 {
		t.Fatalf("event pairing = %d/%d/%d, want 2/2/2", len(prepared), len(started), len(completed))
	}
	for _, p := range prepared {
		if !strings.Contains(string(p), `"prepare_failed":true`) || !strings.Contains(string(p), `"args_raw_hash"`) {
			t.Fatalf("degraded prepared payload = %s", p)
		}
	}
	var errText string
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.Error != nil {
				errText += part.ToolResult.Error.Message + "\n"
			}
		}
	}
	if !strings.Contains(errText, "pattern is required") {
		t.Fatalf("range validation error missing: %q", errText)
	}
	if !strings.Contains(errText, "re-issue the tool call with valid arguments") {
		t.Fatalf("malformed hint missing: %q", errText)
	}
	if strings.Contains(errText, "unknown field") {
		t.Fatalf("internal placeholder field leaked to the model: %q", errText)
	}
}

// E8: compaction masks an oversized output into a readable artifact —
// the placeholder carries an absolute path whose content is the original
// tool output.
func TestE2EMaskExternalizesWithReadablePath(t *testing.T) {
	ws := t.TempDir()
	var big strings.Builder
	for i := 0; i < 400; i++ {
		big.WriteString(strings.Repeat("abcdefghij", 6) + "\n")
	}
	writeFile(t, ws, "big.txt", big.String())
	writeFile(t, ws, "small.txt", "tiny\n")
	// Two parallel reads in one response: the calibrated occupancy
	// (metered 100 + the 27KB big output appended since) crosses the 760
	// trigger right after the batch, and the trailing small result keeps
	// the big one maskable (compaction protects only the final message).
	mock := newMockOpenAI(t, []mockEntry{
		{Tools: []mockToolCall{
			{Name: "read_file", Args: `{"path":"big.txt","limit":500}`},
			{Name: "read_file", Args: `{"path":"small.txt"}`},
		}, UsageIn: 100, UsageOut: 30},
		{Text: "done", UsageIn: 100, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(1000, 200_000, domain.DefaultContextConfig()))

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	compacted := runEvents(run, domain.EventContextCompacted)
	if len(compacted) != 1 || !strings.Contains(string(compacted[0]), `"masked_outputs":1`) {
		t.Fatalf("expected one masked output, got %s", compacted)
	}
	text := mock.requestText(t, 1)
	const marker = "externalized to "
	idx := strings.Index(text, marker)
	if idx < 0 || !strings.Contains(text, "[tool output compacted]") {
		t.Fatalf("mask placeholder missing from the model's input")
	}
	rest := text[idx+len(marker):]
	path := strings.Fields(rest)[0]
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("artifact path %q unreadable: %v", path, err)
	}
	if !strings.Contains(string(data), "abcdefghij") {
		t.Fatalf("artifact does not hold the original output (%d bytes)", len(data))
	}
}

// E9: a plain 429 is a transient rate limit — the real provider stack
// classifies it as retryable and the loop waits out the window instead of
// killing the run (bounded by StartRetryPolicy).
func TestE2ERateLimitRetriedThroughProviderStack(t *testing.T) {
	ws := t.TempDir()
	mock := newMockOpenAI(t, []mockEntry{
		{Fail: true, FailStatus: http.StatusTooManyRequests},
		{Text: "recovered after the rate window", UsageIn: 50, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))
	loop.StartRetry = agent.StartRetryPolicy{
		MaxAttempts: 3, InitialWait: 10 * time.Millisecond, MaxWait: 20 * time.Millisecond, MaxHintWait: 50 * time.Millisecond,
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want the 429 waited out and retried", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if got := mock.requestCount(); got != 2 {
		t.Fatalf("mock requests = %d, want 2 (429 then success)", got)
	}
	retrying := runEvents(run, domain.EventModelRequestRetrying)
	if len(retrying) != 1 || !strings.Contains(string(retrying[0]), `"code":"rate_limited"`) {
		t.Fatalf("expected one rate_limited retrying event, got %s", retrying)
	}
	if failed := runEvents(run, domain.EventModelRequestFailed); len(failed) != 0 {
		t.Fatalf("a recovered retry must not emit request_failed: %s", failed)
	}
}

// E10: a 429 carrying quota-exhaustion text is NOT a transient rate
// limit — no window wait restores an exhausted balance, so the run fails
// fast with a diagnosable quota_exhausted audit.
func TestE2EQuotaExhaustionFailsFast(t *testing.T) {
	ws := t.TempDir()
	mock := newMockOpenAI(t, []mockEntry{
		{
			Fail: true, FailStatus: http.StatusTooManyRequests,
			FailBody: `{"error":{"message":"insufficient_quota: remaining quota is zero"}}`,
		},
		{Text: "unreached", UsageIn: 50, UsageOut: 10},
	})
	registry, artStore := realEnv(t, ws)
	run := newBudgetRun(t, domain.DefaultLimits())
	loop := newRealLoop(run, mock.provider(t), registry, artStore,
		agent.NewWindowModel(200_000, 200_000, domain.DefaultContextConfig()))
	loop.StartRetry = agent.StartRetryPolicy{
		MaxAttempts: 3, InitialWait: 10 * time.Millisecond, MaxWait: 20 * time.Millisecond, MaxHintWait: 50 * time.Millisecond,
	}

	if err := loop.Execute(context.Background()); err == nil {
		t.Fatal("expected quota exhaustion to fail the run")
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	if got := mock.requestCount(); got != 1 {
		t.Fatalf("mock requests = %d, want 1 (quota errors are never retried)", got)
	}
	if retrying := runEvents(run, domain.EventModelRequestRetrying); len(retrying) != 0 {
		t.Fatalf("quota exhaustion must not be retried: %s", retrying)
	}
	failed := runEvents(run, domain.EventModelRequestFailed)
	if len(failed) != 1 || !strings.Contains(string(failed[0]), `"code":"quota_exhausted"`) {
		t.Fatalf("expected one quota_exhausted request_failed, got %s", failed)
	}
}

func countUnresolved(messages []domain.Message) int {
	resolved := make(map[domain.ToolCallID]struct{})
	for _, msg := range messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				resolved[part.ToolResult.CallID] = struct{}{}
			}
		}
	}
	count := 0
	for _, msg := range messages {
		for _, call := range msg.ToolCalls() {
			if _, ok := resolved[call.ID]; !ok {
				count++
			}
		}
	}
	return count
}
