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

package replay

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// stubModel is a full-fidelity scripted model: unlike fakes.FakeModel it
// replays an exact ModelEvent sequence (reasoning, signatures, cached
// tokens, warnings, stream errors), which is the whole point of the
// record/replay layer.
type stubModel struct {
	mu      sync.Mutex
	calls   []domain.ModelRequest
	streams []*stubStream
	// failNext, when non-nil, is returned by the next Stream call.
	failNext error
}

type stubStream struct {
	ctx    context.Context
	events []domain.ModelEvent
	pos    int
	// hang makes Recv block on ctx cancellation once events run out.
	hang bool
}

func (m *stubModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.calls = append(m.calls, req)
	if m.failNext != nil {
		err := m.failNext
		m.failNext = nil
		return nil, err
	}
	if len(m.streams) == 0 {
		return nil, fmt.Errorf("stub model: no scripted stream")
	}
	s := m.streams[0]
	m.streams = m.streams[1:]
	s.ctx = ctx
	return s, nil
}

func (s *stubStream) Recv() (domain.ModelEvent, error) {
	if s.pos < len(s.events) {
		evt := s.events[s.pos]
		s.pos++
		return evt, nil
	}
	if s.hang {
		<-s.ctx.Done()
		return domain.ModelEvent{}, s.ctx.Err()
	}
	return domain.ModelEvent{}, io.EOF
}

func (s *stubStream) Close() error { return nil }

func drain(t *testing.T, stream domain.ModelStream) []domain.ModelEvent {
	t.Helper()
	var out []domain.ModelEvent
	for {
		evt, err := stream.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return out
			}
			t.Fatalf("Recv: %v", err)
		}
		out = append(out, evt)
	}
}

func mustToolCallID(t *testing.T, s string) domain.ToolCallID {
	t.Helper()
	id, err := domain.ParseToolCallID(s)
	if err != nil {
		t.Fatalf("ParseToolCallID: %v", err)
	}
	return id
}

func sampleRequest(text string) domain.ModelRequest {
	return domain.ModelRequest{
		ModelName: "test-model",
		Messages: []domain.Message{{
			ID:    domain.NewMessageID(),
			Role:  domain.RoleUser,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		}},
		MaxTokens:   1024,
		Temperature: 0.5,
		Reasoning:   domain.ReasoningSpec{Effort: "high"},
		Tools: []domain.ToolDefinition{{
			Name:        "read_file",
			Description: "read a file",
			InputSchema: []byte(`{"type":"object","properties":{"path":{"type":"string"}}}`),
		}},
	}
}

// fullFidelityEvents covers every event kind the recording must carry
// losslessly — the ones fakes.ScriptEntry cannot express.
func fullFidelityEvents() []domain.ModelEvent {
	return []domain.ModelEvent{
		{Kind: domain.ModelEventResponseStart},
		{Kind: domain.ModelEventReasoningStart},
		{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "thinking..."},
		{Kind: domain.ModelEventReasoningEnd, ReasoningSignature: "sig-abc", ReasoningRedacted: false},
		{Kind: domain.ModelEventTextStart},
		{Kind: domain.ModelEventTextDelta, TextDelta: "answer"},
		{Kind: domain.ModelEventTextEnd},
		{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: "call_1", ToolName: "read_file"},
		{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: `{"path":"a.txt"}`},
		{Kind: domain.ModelEventToolCallEnd, ToolIndex: 0},
		{Kind: domain.ModelEventUsage, InputTokens: 100, OutputTokens: 20, CachedInputTokens: 80},
		{Kind: domain.ModelEventProviderWarning, Error: "warning: slow"},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse},
	}
}

func TestRecordReplayRoundTrip(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	// Record two calls against the stub "provider".
	inner := &stubModel{streams: []*stubStream{
		{events: fullFidelityEvents()},
		{events: []domain.ModelEvent{
			{Kind: domain.ModelEventTextStart},
			{Kind: domain.ModelEventTextDelta, TextDelta: "second"},
			{Kind: domain.ModelEventTextEnd},
			{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
		}},
	}}
	rec, err := NewRecorder(dir)
	if err != nil {
		t.Fatalf("NewRecorder: %v", err)
	}
	model := rec.Wrap(inner)
	for _, prompt := range []string{"first", "second"} {
		stream, err := model.Stream(ctx, sampleRequest(prompt))
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}
		drain(t, stream)
		if err := stream.Close(); err != nil {
			t.Fatalf("Close: %v", err)
		}
	}
	if err := rec.Close(); err != nil {
		t.Fatalf("recorder Close: %v", err)
	}

	// The recording is the single root file and carries full fidelity.
	raw, err := os.ReadFile(filepath.Join(dir, CallsFileName))
	if err != nil {
		t.Fatalf("read recording: %v", err)
	}
	for _, want := range []string{
		`"type":"call_start"`, `"kind":"reasoning_end"`, `"reasoning_signature":"sig-abc"`,
		`"cached_input_tokens":80`, `"kind":"provider_warning"`, `"type":"call_end"`,
	} {
		if !strings.Contains(string(raw), want) {
			t.Fatalf("recording misses %s:\n%s", want, raw)
		}
	}

	// Replay reproduces the exact event sequences, positionally.
	replayModel, err := NewReplayModel(dir)
	if err != nil {
		t.Fatalf("NewReplayModel: %v", err)
	}
	stream, err := replayModel.Stream(ctx, sampleRequest("first"))
	if err != nil {
		t.Fatalf("replay Stream #1: %v", err)
	}
	got := drain(t, stream)
	want := fullFidelityEvents()
	if fmt.Sprintf("%v", got) != fmt.Sprintf("%v", want) {
		t.Fatalf("replayed events diverge:\ngot  %v\nwant %v", got, want)
	}
	stream2, err := replayModel.Stream(ctx, sampleRequest("second"))
	if err != nil {
		t.Fatalf("replay Stream #2: %v", err)
	}
	drain(t, stream2)

	if err := replayModel.AssertConsumed(); err != nil {
		t.Fatalf("AssertConsumed: %v", err)
	}

	// A third call exceeds the recording: script exhausted.
	if _, err := replayModel.Stream(ctx, sampleRequest("third")); err == nil ||
		!strings.Contains(err.Error(), "script exhausted") {
		t.Fatalf("extra call err = %v, want script exhausted", err)
	}
}

func TestReplayDetectsUnderConsumption(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	inner := &stubModel{streams: []*stubStream{{events: fullFidelityEvents()}, {events: fullFidelityEvents()}}}
	rec, _ := NewRecorder(dir)
	model := rec.Wrap(inner)
	for i := 0; i < 2; i++ {
		stream, _ := model.Stream(ctx, sampleRequest("x"))
		drain(t, stream)
		_ = stream.Close()
	}
	_ = rec.Close()

	replayModel, err := NewReplayModel(dir)
	if err != nil {
		t.Fatalf("NewReplayModel: %v", err)
	}
	stream, _ := replayModel.Stream(ctx, sampleRequest("x"))
	drain(t, stream)
	// The agent under test stopped after one call; the second recorded
	// call was never consumed.
	if err := replayModel.AssertConsumed(); err == nil ||
		!strings.Contains(err.Error(), "consumed 1/2") {
		t.Fatalf("AssertConsumed = %v, want under-consumption report", err)
	}
}

func TestReplayRejectsUnrecordedSession(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	inner := &stubModel{streams: []*stubStream{{events: fullFidelityEvents()}}}
	rec, _ := NewRecorder(dir)
	stream, _ := rec.Wrap(inner).Stream(ctx, sampleRequest("x"))
	drain(t, stream)
	_ = stream.Close()
	_ = rec.Close()

	replayModel, err := NewReplayModel(dir)
	if err != nil {
		t.Fatalf("NewReplayModel: %v", err)
	}
	subCtx := WithSessionRef(ctx, domain.NewSessionID(), mustToolCallID(t, "call_999"))
	if _, err := replayModel.Stream(subCtx, sampleRequest("x")); err == nil ||
		!strings.Contains(err.Error(), "unrecorded session") {
		t.Fatalf("err = %v, want unrecorded session", err)
	}
}

func TestSubsessionShardingRoundTrip(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	// One root call and one delegated call, interleaved construction to
	// prove the sharding (not the global order) carries the binding.
	inner := &stubModel{streams: []*stubStream{
		{events: []domain.ModelEvent{{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse}}},
		{events: []domain.ModelEvent{{Kind: domain.ModelEventTextDelta, TextDelta: "child answer"}}},
	}}
	rec, _ := NewRecorder(dir)
	model := rec.Wrap(inner)
	rootStream, _ := model.Stream(ctx, sampleRequest("root"))
	subCtx := WithSessionRef(ctx, domain.NewSessionID(), mustToolCallID(t, "call_delegate_1"))
	subStream, _ := model.Stream(subCtx, sampleRequest("child"))
	drain(t, rootStream)
	drain(t, subStream)
	_ = rootStream.Close()
	_ = subStream.Close()
	if err := rec.Close(); err != nil {
		t.Fatalf("recorder Close: %v", err)
	}

	for _, name := range []string{CallsFileName, CallsFileFor("call_delegate_1")} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("missing recording %s: %v", name, err)
		}
	}

	replayModel, err := NewReplayModel(dir)
	if err != nil {
		t.Fatalf("NewReplayModel: %v", err)
	}
	// The child binds by its parent tool call ID regardless of order.
	gotSub, _ := replayModel.Stream(subCtx, sampleRequest("child"))
	events := drain(t, gotSub)
	if len(events) != 1 || events[0].TextDelta != "child answer" {
		t.Fatalf("child replay = %v", events)
	}
	gotRoot, _ := replayModel.Stream(ctx, sampleRequest("root"))
	drain(t, gotRoot)
	if err := replayModel.AssertConsumed(); err != nil {
		t.Fatalf("AssertConsumed: %v", err)
	}
}

func TestStreamErrorRoundTrip(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	// The stream dies mid-flight with a provider error; the prefix
	// events and the error must both survive the round trip.
	inner := &stubModel{streams: []*stubStream{{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextDelta, TextDelta: "partial"},
		{Kind: domain.ModelEventStreamError, Error: "connection reset", Retryable: true},
	}}}}
	rec, _ := NewRecorder(dir)
	stream, _ := rec.Wrap(inner).Stream(ctx, sampleRequest("x"))
	got := drain(t, stream)
	_ = stream.Close()
	if len(got) != 2 || got[1].Kind != domain.ModelEventStreamError {
		t.Fatalf("recorded events = %v", got)
	}
	_ = rec.Close()

	replayModel, _ := NewReplayModel(dir)
	replayed, _ := replayModel.Stream(ctx, sampleRequest("x"))
	got = drain(t, replayed)
	if len(got) != 2 || got[1].Error != "connection reset" || !got[1].Retryable {
		t.Fatalf("replayed events = %v", got)
	}
	if err := replayModel.AssertConsumed(); err != nil {
		t.Fatalf("AssertConsumed: %v", err)
	}
}

func TestStreamStartFailureRoundTrip(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	inner := &stubModel{failNext: fmt.Errorf("provider 401: bad key")}
	rec, _ := NewRecorder(dir)
	if _, err := rec.Wrap(inner).Stream(ctx, sampleRequest("x")); err == nil {
		t.Fatal("expected the start failure")
	}
	_ = rec.Close()

	replayModel, _ := NewReplayModel(dir)
	stream, err := replayModel.Stream(ctx, sampleRequest("x"))
	if err != nil {
		t.Fatalf("replay Stream: %v", err)
	}
	if _, err := stream.Recv(); err == nil || !strings.Contains(err.Error(), "provider 401") {
		t.Fatalf("Recv err = %v, want the recorded provider error", err)
	}
	if err := replayModel.AssertConsumed(); err != nil {
		t.Fatalf("AssertConsumed: %v", err)
	}
}

func TestHangRoundTrip(t *testing.T) {
	dir := t.TempDir()

	// Record a call that blocks until cancellation (steering/cancel).
	inner := &stubModel{streams: []*stubStream{{
		events: []domain.ModelEvent{{Kind: domain.ModelEventTextDelta, TextDelta: "partial"}},
		hang:   true,
	}}}
	rec, _ := NewRecorder(dir)
	callCtx, cancel := context.WithCancel(context.Background())
	stream, _ := rec.Wrap(inner).Stream(callCtx, sampleRequest("x"))
	if _, err := stream.Recv(); err != nil {
		t.Fatalf("Recv prefix: %v", err)
	}
	done := make(chan error, 1)
	go func() {
		_, err := stream.Recv()
		done <- err
	}()
	time.Sleep(20 * time.Millisecond)
	cancel()
	if err := <-done; !errors.Is(err, context.Canceled) {
		t.Fatalf("hang Recv err = %v, want context.Canceled", err)
	}
	_ = stream.Close()
	_ = rec.Close()

	raw, _ := os.ReadFile(filepath.Join(dir, CallsFileName))
	if !strings.Contains(string(raw), `"type":"call_hang"`) {
		t.Fatalf("recording misses call_hang:\n%s", raw)
	}

	// Replay: the prefix replays, then the stream blocks until cancel;
	// AssertConsumed is clean because the cancel really happened.
	replayModel, _ := NewReplayModel(dir)
	callCtx2, cancel2 := context.WithCancel(context.Background())
	replayed, _ := replayModel.Stream(callCtx2, sampleRequest("x"))
	if _, err := replayed.Recv(); err != nil {
		t.Fatalf("replay prefix: %v", err)
	}
	done2 := make(chan error, 1)
	go func() {
		_, err := replayed.Recv()
		done2 <- err
	}()
	select {
	case <-done2:
		t.Fatal("replayed hang returned before cancellation")
	case <-time.After(20 * time.Millisecond):
	}
	cancel2()
	if err := <-done2; !errors.Is(err, context.Canceled) {
		t.Fatalf("replayed hang err = %v, want context.Canceled", err)
	}
	_ = replayed.Close()
	if err := replayModel.AssertConsumed(); err != nil {
		t.Fatalf("AssertConsumed: %v", err)
	}
}

func TestHangClosedWithoutCancelIsReported(t *testing.T) {
	dir := t.TempDir()

	inner := &stubModel{streams: []*stubStream{{hang: true}}}
	rec, _ := NewRecorder(dir)
	callCtx, cancel := context.WithCancel(context.Background())
	stream, _ := rec.Wrap(inner).Stream(callCtx, sampleRequest("x"))
	cancel()
	if _, err := stream.Recv(); !errors.Is(err, context.Canceled) {
		t.Fatalf("Recv err = %v", err)
	}
	_ = stream.Close()
	_ = rec.Close()

	replayModel, _ := NewReplayModel(dir)
	replayed, _ := replayModel.Stream(context.Background(), sampleRequest("x"))
	// Reach the hang position on a never-cancelled context, then abandon
	// the stream with Close: the cancel semantics under test regressed.
	go func() { _, _ = replayed.Recv() }()
	time.Sleep(20 * time.Millisecond)
	_ = replayed.Close()
	if err := replayModel.AssertConsumed(); err == nil ||
		!strings.Contains(err.Error(), "closed without cancellation") {
		t.Fatalf("AssertConsumed = %v, want the hang violation", err)
	}
}

func TestFingerprintDriftWarnsAndStrictFails(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	inner := &stubModel{streams: []*stubStream{{events: fullFidelityEvents()}}}
	rec, _ := NewRecorder(dir)
	stream, _ := rec.Wrap(inner).Stream(ctx, sampleRequest("original prompt"))
	drain(t, stream)
	_ = stream.Close()
	_ = rec.Close()

	// Non-strict: a changed prompt warns but replays.
	var warnings []string
	replayModel, _ := NewReplayModel(dir, WithWarnFunc(func(format string, args ...any) {
		warnings = append(warnings, fmt.Sprintf(format, args...))
	}))
	replayed, err := replayModel.Stream(ctx, sampleRequest("EDITED prompt"))
	if err != nil {
		t.Fatalf("replay Stream: %v", err)
	}
	drain(t, replayed)
	if len(warnings) != 1 || !strings.Contains(warnings[0], "fingerprint drifted") {
		t.Fatalf("warnings = %v, want one fingerprint drift warning", warnings)
	}

	// Strict: the same drift fails the call.
	strict, _ := NewReplayModel(dir, WithStrict(true))
	if _, err := strict.Stream(ctx, sampleRequest("EDITED prompt")); err == nil ||
		!strings.Contains(err.Error(), "fingerprint drifted") {
		t.Fatalf("strict err = %v, want fingerprint failure", err)
	}

	// An unchanged request is silent.
	quiet, _ := NewReplayModel(dir, WithWarnFunc(func(format string, args ...any) {
		t.Fatalf("unexpected warning: "+format, args...)
	}))
	replayed2, _ := quiet.Stream(ctx, sampleRequest("original prompt"))
	drain(t, replayed2)
}

func TestFingerprintScrubPaths(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	// The record run's workspace path lives in the request; replay runs
	// in a different temp dir, so both sides scrub the root.
	withPath := func(root string) domain.ModelRequest {
		req := sampleRequest("read it")
		req.Messages[0].Parts = append(req.Messages[0].Parts, domain.ContentPart{
			Kind: domain.PartText, Text: "workspace is " + root,
		})
		return req
	}

	rec, _ := NewRecorder(dir, WithScrubPaths(ScrubPath{Prefix: "/tmp/record-ws", Token: "{{cwd}}"}))
	stream, _ := rec.Wrap(&stubModel{streams: []*stubStream{{events: fullFidelityEvents()}}}).Stream(ctx, withPath("/tmp/record-ws"))
	drain(t, stream)
	_ = stream.Close()
	_ = rec.Close()

	var warnings []string
	replayModel, _ := NewReplayModel(
		dir,
		WithScrubPaths(ScrubPath{Prefix: "/tmp/replay-ws", Token: "{{cwd}}"}),
		WithWarnFunc(func(format string, args ...any) { warnings = append(warnings, fmt.Sprintf(format, args...)) }),
	)
	replayed, _ := replayModel.Stream(ctx, withPath("/tmp/replay-ws"))
	drain(t, replayed)
	if len(warnings) != 0 {
		t.Fatalf("scrubbed paths still drifted: %v", warnings)
	}

	// Without scrubbing the path difference surfaces as drift.
	var warnings2 []string
	raw, _ := NewReplayModel(dir, WithWarnFunc(func(format string, args ...any) {
		warnings2 = append(warnings2, fmt.Sprintf(format, args...))
	}))
	replayed2, _ := raw.Stream(ctx, withPath("/tmp/replay-ws"))
	drain(t, replayed2)
	if len(warnings2) != 1 {
		t.Fatalf("warnings = %v, want one drift warning", warnings2)
	}
}

func TestLoadScriptRejectsTruncatedRecording(t *testing.T) {
	dir := t.TempDir()
	// call_start with no terminator: the recorder died mid-call.
	if err := os.WriteFile(filepath.Join(dir, CallsFileName), []byte(
		`{"type":"call_start","seq":1,"fingerprint":"sha256:x","request":{}}`+"\n",
	), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewReplayModel(dir); err == nil || !strings.Contains(err.Error(), "no terminator") {
		t.Fatalf("err = %v, want truncation report", err)
	}
}
