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

package app

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

var mazeBase = time.Date(2026, 8, 21, 10, 0, 0, 0, time.UTC)

func mazeEvent(t *testing.T, seq int64, typ domain.EventType, payload any, at time.Time) domain.Event {
	t.Helper()
	raw, err := domain.MarshalPayload(payload)
	if err != nil {
		t.Fatalf("marshal payload: %v", err)
	}
	return domain.Event{
		ID: domain.NewEventID(), Sequence: seq,
		SessionID: domain.NewSessionID(), Type: typ,
		Timestamp: at, Payload: raw,
	}
}

func mazeUserMsg(text string) domain.MessageEventPayload {
	return domain.MessageEventPayload{Message: domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: text}},
	}}
}

func mazeAssistantMsg(t *testing.T, seq int64, requestID domain.EventID, reasoning string, calls ...domain.ToolCall) domain.ResponseCompletedPayload {
	t.Helper()
	parts := []domain.ContentPart{}
	if reasoning != "" {
		parts = append(parts, domain.ContentPart{Kind: domain.PartReasoning, Reasoning: &domain.ReasoningContent{Text: reasoning}})
	}
	for i := range calls {
		parts = append(parts, domain.ContentPart{Kind: domain.PartToolCall, ToolCall: &calls[i]})
	}
	return domain.ResponseCompletedPayload{
		MessageEventPayload: domain.MessageEventPayload{Message: domain.Message{
			ID: domain.NewMessageID(), Sequence: seq, Role: domain.RoleAssistant,
			Status: domain.MessageStatusFinal, Revision: 1, Parts: parts,
			Metadata: map[string]string{"request_id": requestID.String()},
		}},
		Usage: &domain.RequestUsage{InputTokens: 1000, OutputTokens: 50, ReasoningTokens: 20},
	}
}

func mazeToolCall(t *testing.T, name, args string) domain.ToolCall {
	t.Helper()
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: json.RawMessage(args)}
}

func mazeToolResultMsg(callID domain.ToolCallID, status domain.ToolStatus, text string) domain.MessageEventPayload {
	return domain.MessageEventPayload{Message: domain.Message{
		ID: domain.NewMessageID(), Sequence: 99, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
			CallID:  callID,
			Status:  status,
			Content: []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		}}},
	}}
}

// A full happy-path step: request → response (with a tool call) → result.
func TestBuildMazeBasicFlow(t *testing.T) {
	reqID := domain.NewEventID()
	call := mazeToolCall(t, "read_file", `{"path":"a.go"}`)
	ts := mazeBase
	events := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("修个 bug"), ts),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "glm-5.3", Turn: 1}, ts.Add(time.Second)),
		mazeEvent(t, 3, domain.EventModelResponseCompleted, mazeAssistantMsg(t, 2, reqID, "先看看代码", call), ts.Add(3*time.Second)),
		mazeEvent(t, 4, domain.EventToolResultAdded, mazeToolResultMsg(call.ID, domain.ToolStatusSuccess, "package main"), ts.Add(4*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "测试", events, nil, ts.Add(10*time.Second), false)
	if len(data.Lanes) != 1 {
		t.Fatalf("lanes = %d, want 1", len(data.Lanes))
	}
	lane := data.Lanes[0]
	if lane.Model != "glm-5.3" {
		t.Fatalf("model = %q, want glm-5.3", lane.Model)
	}
	if len(lane.Main) != 1 || len(lane.Detours) != 0 {
		t.Fatalf("main/detours = %d/%d, want 1/0", len(lane.Main), len(lane.Detours))
	}
	step := lane.Main[0]
	if step.Turn != 1 || step.V != "ok" {
		t.Fatalf("step turn/v = %d/%q, want 1/ok", step.Turn, step.V)
	}
	if len(step.Tools) != 1 || step.Tools[0].Name != "read_file" || step.Tools[0].V != "ok" {
		t.Fatalf("tool = %+v", step.Tools)
	}
	if step.Tools[0].Args != "a.go" {
		t.Fatalf("args summary = %q, want a.go", step.Tools[0].Args)
	}
	if step.Rz != 1 || step.RzTxt == "" {
		t.Fatalf("reasoning = %d/%q", step.Rz, step.RzTxt)
	}
	if step.InTok == nil || *step.InTok != 1000 || step.OutTok == nil || *step.OutTok != 50 {
		t.Fatalf("per-request usage missing: %+v", step)
	}
	if step.RzTok == nil || *step.RzTok != 20 {
		t.Fatalf("reasoning tokens missing: %+v", step)
	}
	// The axis anchors at the first user message: the step starts at +1s.
	if step.S != 1 || step.E != 4 {
		t.Fatalf("step span = %v~%v, want 1~4", step.S, step.E)
	}
}

// A failed tool sends its step into a detour; a tool-less answer stays
// on the main path.
func TestBuildMazeErrorDetourAndAnswer(t *testing.T) {
	req1, req2 := domain.NewEventID(), domain.NewEventID()
	call1 := mazeToolCall(t, "run_cmd", `{"command":"go build"}`)
	ts := mazeBase
	events := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("构建一下"), ts),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: req1, ModelName: "m", Turn: 1}, ts.Add(time.Second)),
		mazeEvent(t, 3, domain.EventModelResponseCompleted, mazeAssistantMsg(t, 2, req1, "", call1), ts.Add(2*time.Second)),
		mazeEvent(t, 4, domain.EventToolResultAdded, mazeToolResultMsg(call1.ID, domain.ToolStatusError, "exit code 1"), ts.Add(3*time.Second)),
		mazeEvent(t, 5, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: req2, ModelName: "m", Turn: 1}, ts.Add(4*time.Second)),
		mazeEvent(t, 6, domain.EventModelResponseCompleted, mazeAssistantMsg(t, 4, req2, "修复完毕"), ts.Add(6*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, ts.Add(10*time.Second), false)
	lane := data.Lanes[0]
	if len(lane.Detours) != 1 || lane.Detours[0].V != "error" {
		t.Fatalf("detours = %+v", lane.Detours)
	}
	if len(lane.Main) != 1 || lane.Main[0].V != "answer" {
		t.Fatalf("main = %+v", lane.Main)
	}
	// The detour attaches to the nearest main step before the failure;
	// none exists yet, so attach = 0.
	if lane.Detours[0].Attach != 0 {
		t.Fatalf("attach = %d, want 0", lane.Detours[0].Attach)
	}
}

// A search tool that succeeds with an empty result is a dead-end detour;
// a write tool's short confirmation must not be framed as one.
func TestMazeVerdictDeadendVsWriteOk(t *testing.T) {
	grepCall := mazeToolCall(t, "grep", `{"pattern":"foo"}`)
	writeCall := mazeToolCall(t, "write", `{"path":"b.go"}`)
	reqID := domain.NewEventID()
	ts := mazeBase
	events := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("找一下"), ts),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "m", Turn: 1}, ts.Add(time.Second)),
		mazeEvent(t, 3, domain.EventModelResponseCompleted, mazeAssistantMsg(t, 2, reqID, "", grepCall, writeCall), ts.Add(2*time.Second)),
		mazeEvent(t, 4, domain.EventToolResultAdded, mazeToolResultMsg(grepCall.ID, domain.ToolStatusSuccess, "no matches found"), ts.Add(3*time.Second)),
		mazeEvent(t, 5, domain.EventToolResultAdded, mazeToolResultMsg(writeCall.ID, domain.ToolStatusSuccess, "ok"), ts.Add(4*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, ts.Add(10*time.Second), false)
	lane := data.Lanes[0]
	if len(lane.Detours) != 1 || lane.Detours[0].V != "deadend" {
		t.Fatalf("step verdict = %+v, want deadend detour", lane.Detours)
	}
	step := lane.Detours[0]
	var grepTool, writeTool *MazeTool
	for i := range step.Tools {
		switch step.Tools[i].Name {
		case "grep":
			grepTool = &step.Tools[i]
		case "write":
			writeTool = &step.Tools[i]
		}
	}
	if grepTool == nil || grepTool.V != "deadend" {
		t.Fatalf("grep verdict = %+v", grepTool)
	}
	if writeTool == nil || writeTool.V != "ok" {
		t.Fatalf("write verdict = %+v (a short success confirmation must stay ok)", writeTool)
	}
}

// Blind retry: a consecutive same-tool cluster with similar arguments and
// a failure inside re-marks its non-failed members as retries.
func TestMazeRetryCluster(t *testing.T) {
	ts := mazeBase
	var events []domain.Event
	seq := int64(1)
	events = append(events, mazeEvent(t, seq, domain.EventUserMessageAdded, mazeUserMsg("跑起来"), ts))
	seq++
	// Three consecutive near-identical run_cmd calls; the first fails.
	statuses := []domain.ToolStatus{domain.ToolStatusError, domain.ToolStatusSuccess, domain.ToolStatusSuccess}
	for i, status := range statuses {
		reqID := domain.NewEventID()
		call := mazeToolCall(t, "run_cmd", `{"command":"npm run build"}`)
		at := ts.Add(time.Duration(1+i*3) * time.Second)
		events = append(
			events,
			mazeEvent(t, seq, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "m", Turn: 1}, at),
			mazeEvent(t, seq+1, domain.EventModelResponseCompleted, mazeAssistantMsg(t, seq+1, reqID, "", call), at.Add(time.Second)),
			mazeEvent(t, seq+2, domain.EventToolResultAdded, mazeToolResultMsg(call.ID, status, "output"), at.Add(2*time.Second)),
		)
		seq += 3
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, ts.Add(30*time.Second), false)
	lane := data.Lanes[0]
	// All three steps become detours: error + retry + retry.
	if len(lane.Detours) != 3 {
		t.Fatalf("detours = %d, want 3 (main=%d)", len(lane.Detours), len(lane.Main))
	}
	want := []string{"error", "retry", "retry"}
	for i, d := range lane.Detours {
		if d.V != want[i] {
			t.Fatalf("detour %d v = %q, want %q", i, d.V, want[i])
		}
	}
}

// An in-flight step (request_started without a response) renders as a
// live main-path step and the axis extends to now.
func TestMazeLiveStep(t *testing.T) {
	reqID := domain.NewEventID()
	ts := mazeBase
	now := ts.Add(90 * time.Second)
	events := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("hi"), ts),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "m", Turn: 1}, ts.Add(80*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, now, true)
	lane := data.Lanes[0]
	if len(lane.Main) != 1 || !lane.Main[0].Live {
		t.Fatalf("main = %+v, want one live step", lane.Main)
	}
	if lane.Main[0].E != 90 {
		t.Fatalf("live step e = %v, want 90 (now)", lane.Main[0].E)
	}
}

// A dead session's unanswered request must not smear across the axis:
// without a live controller the step keeps its last known end.
func TestMazeDeadSessionNoLiveExtension(t *testing.T) {
	reqID := domain.NewEventID()
	ts := mazeBase
	events := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("hi"), ts),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "m", Turn: 1}, ts.Add(80*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, ts.Add(90*time.Second), false)
	lane := data.Lanes[0]
	if len(lane.Main) != 1 {
		t.Fatalf("main = %+v, want one step", lane.Main)
	}
	if lane.Main[0].Live {
		t.Fatalf("dead session step must not be live")
	}
	if lane.Main[0].V != "pending" {
		t.Fatalf("dead step v = %q, want pending (not a fake answer)", lane.Main[0].V)
	}
	if lane.Main[0].E != 80 {
		t.Fatalf("dead step e = %v, want 80 (request time, not now)", lane.Main[0].E)
	}
}

// Legacy fallback: request_started events without a turn field split turns
// by user-message count.
func TestMazeTurnFallbackForLegacyLogs(t *testing.T) {
	ts := mazeBase
	var events []domain.Event
	seq := int64(1)
	for turn := 1; turn <= 2; turn++ {
		reqID := domain.NewEventID()
		at := ts.Add(time.Duration(turn*10) * time.Second)
		events = append(
			events,
			mazeEvent(t, seq, domain.EventUserMessageAdded, mazeUserMsg("第N轮"), at),
			mazeEvent(t, seq+1, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "m"}, at.Add(time.Second)),
			mazeEvent(t, seq+2, domain.EventModelResponseCompleted, mazeAssistantMsg(t, seq+2, reqID, "答"), at.Add(2*time.Second)),
		)
		seq += 3
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, ts.Add(60*time.Second), false)
	lane := data.Lanes[0]
	if len(lane.Main) != 2 || lane.Main[0].Turn != 1 || lane.Main[1].Turn != 2 {
		t.Fatalf("turns = %+v, want 1/2", lane.Main)
	}
}

// A sub-agent session folds into one aggregated detour node sharing the
// parent's clock.
func TestMazeSubagentFold(t *testing.T) {
	ts := mazeBase
	childReq := domain.NewEventID()
	childEvents := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("子任务"), ts.Add(5*time.Second)),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: childReq, ModelName: "m", Turn: 1}, ts.Add(6*time.Second)),
		mazeEvent(t, 3, domain.EventModelResponseCompleted, mazeAssistantMsg(t, 2, childReq, "子代理完成"), ts.Add(20*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "", []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("父任务"), ts),
	}, []mazeChild{{Label: "子任务摘要", Running: false, Events: childEvents}}, ts.Add(60*time.Second), false)
	lane := data.Lanes[0]
	if len(lane.Detours) != 1 || !lane.Detours[0].Sub {
		t.Fatalf("detours = %+v, want one sub node", lane.Detours)
	}
	sub := lane.Detours[0]
	// The sub node's span is the child's step-activity span (first request
	// through last response).
	if sub.Label != "子任务摘要" || sub.S != 6 || sub.E != 20 {
		t.Fatalf("sub node = label %q span %v~%v, want 6~20", sub.Label, sub.S, sub.E)
	}
	if sub.V != "ok" {
		t.Fatalf("sub verdict = %q, want ok", sub.V)
	}
}

// A step whose model request fails (unretryable) becomes a detour.
func TestMazeRequestFailed(t *testing.T) {
	reqID := domain.NewEventID()
	ts := mazeBase
	events := []domain.Event{
		mazeEvent(t, 1, domain.EventUserMessageAdded, mazeUserMsg("hi"), ts),
		mazeEvent(t, 2, domain.EventModelRequestStarted, mazeRequestStartedDTO{RequestID: reqID, ModelName: "m", Turn: 1}, ts.Add(time.Second)),
		mazeEvent(t, 3, domain.EventModelRequestFailed, mazeRequestFailedDTO{RequestID: reqID, Stage: "start", Code: "unavailable", Message: "connection refused"}, ts.Add(3*time.Second)),
	}
	data := BuildMaze(domain.NewSessionID(), "", events, nil, ts.Add(10*time.Second), false)
	lane := data.Lanes[0]
	if len(lane.Detours) != 1 || lane.Detours[0].V != "error" {
		t.Fatalf("detours = %+v, want one error detour", lane.Detours)
	}
	if lane.Detours[0].Why == "" {
		t.Fatalf("failed step should carry a reason")
	}
}
