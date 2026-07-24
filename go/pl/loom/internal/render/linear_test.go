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
// Created: 2026/07/23

package render

import (
	"bytes"
	"encoding/json"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

func TestLinear_ObserveEphemeral_TextDelta(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindModelTextDelta,
		Time:      now,
		Durable:   false,
	}
	evt.Payload = mustMarshal(t, runtimeevent.ModelTextDeltaPayload{
		RequestID: domain.NewEventID(),
		Delta:     "Hello, world!",
	})
	r.ObserveEphemeral(evt)
	assert.Equal(t, "Hello, world!", buf.String())
}

func TestLinear_ObserveEphemeral_IgnoresNonDelta(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()

	// Tool progress should not produce output
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindToolProgress,
		Time:      now,
		Durable:   false,
	}
	evt.Payload = mustMarshal(t, runtimeevent.ToolProgressPayload{
		CallID:   domain.NewToolCallID(),
		Stage:    "executing",
		Progress: 0.5,
	})
	r.ObserveEphemeral(evt)
	assert.Empty(t, buf.String())

	// Tool call delta should not produce output
	evt2 := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  2,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindModelToolCallDelta,
		Time:      now,
		Durable:   false,
	}
	evt2.Payload = mustMarshal(t, runtimeevent.ModelToolCallDeltaPayload{
		RequestID:  domain.NewEventID(),
		DeltaBytes: 10,
	})
	r.ObserveEphemeral(evt2)
	assert.Empty(t, buf.String())
}

func TestLinear_ObserveDurable_TurnStarted(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindTurnStarted,
		Time:      now,
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.TurnStartedPayload{
		TurnIndex: 1,
		Prompt:    "hello",
	})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[TURN 1] hello")
}

func TestLinear_ObserveDurable_ModelRequestStarted(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindModelRequestStarted,
		Time:      now,
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.ModelRequestStartedPayload{
		RequestID: domain.NewEventID(),
		ModelName: "test-model",
		Turn:      1,
	})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[assistant]")
}

func TestLinear_ObserveDurable_ToolCompleted(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()

	// Success case
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindToolCompleted,
		Time:      now,
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.ToolCompletedPayload{
		CallID:     domain.NewToolCallID(),
		ToolName:   "read_file",
		Status:     domain.ToolStatusSuccess,
		DurationMs: 42,
	})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[tool] read_file completed (42ms)")

	// Error case
	buf.Reset()
	evt2 := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  2,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindToolCompleted,
		Time:      now,
		Durable:   true,
	}
	evt2.Payload = mustMarshal(t, runtimeevent.ToolCompletedPayload{
		CallID:     domain.NewToolCallID(),
		ToolName:   "run_cmd",
		Status:     domain.ToolStatusError,
		DurationMs: 100,
		Error:      "command not found",
	})
	err = r.ObserveDurable(evt2)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[tool] run_cmd error: command not found (100ms)")
}

func TestLinear_ObserveDurable_ApprovalRequested(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindApprovalRequested,
		Time:      now,
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.ApprovalRequestedPayload{
		ApprovalID:  domain.NewEventID(),
		CallID:      domain.NewToolCallID(),
		ToolName:    "run_cmd",
		Risk:        domain.R3,
		Description: "Run a shell command",
		ArgsHash:    "abc123",
	})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	output := buf.String()
	assert.Contains(t, output, "[approval needed] run_cmd")
	assert.Contains(t, output, "DENIED (non-TTY mode")
}

func TestLinear_ObserveDurable_ModelRequestFailed(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindModelRequestFailed,
		Time:      now,
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.ModelRequestFailedPayload{
		RequestID: domain.NewEventID(),
		Stage:     "stream",
		Code:      "rate_limited",
	})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[model error] stream: rate_limited")
}

func TestLinear_ObserveDurable_RunCancelled(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindRunCancelled,
		Time:      now,
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.RunCancelledPayload{
		Reason: "user requested cancel",
	})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[cancelled] user requested cancel")
}

func TestLinear_ObserveDurable_RunCompleted(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindRunCompleted,
		Time:      now,
		Durable:   true,
	}
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
	assert.Contains(t, buf.String(), "[completed]")
}

func TestLinear_NoDurableEventDropped(t *testing.T) {
	// Verify that ObserveDurable never returns an error — all events
	// are consumed without erroring, even unknown events.
	var buf bytes.Buffer
	r := NewLinear(&buf)
	evt := runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindRunPhaseChanged,
		Time:      time.Now(),
		Durable:   true,
	}
	evt.Payload = mustMarshal(t, runtimeevent.RunPhasePayload{Phase: domain.PhaseCallingModel})
	err := r.ObserveDurable(evt)
	require.NoError(t, err)
}

func TestLinear_ReportError(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	r.ReportError("something went wrong")
	assert.Contains(t, buf.String(), "[error] something went wrong")
}

func TestRiskDescription(t *testing.T) {
	tests := []struct {
		risk domain.RiskLevel
		want string
	}{
		{domain.R0, "none"},
		{domain.R1, "read-only"},
		{domain.R2, "write"},
		{domain.R3, "destructive"},
		{domain.R4, "critical"},
		{domain.RiskLevel(99), "unknown(99)"},
	}
	for _, tt := range tests {
		t.Run(tt.want, func(t *testing.T) {
			assert.Equal(t, tt.want, RiskDescription(tt.risk))
		})
	}
}

func TestSanitizeText(t *testing.T) {
	tests := []struct {
		name string
		in   string
		want string
	}{
		{"plain", "hello world", "hello world"},
		{"preserve newline", "line1\nline2", "line1\nline2"},
		{"preserve tab", "col1\tcol2", "col1\tcol2"},
		{"drop cr", "line\r\n", "line\n"},
		{"drop c1", "hello\u0085world", "helloworld"},
		{"drop bell", "hello\x07world", "helloworld"},
		{"drop escape", "hello\x1b[31mworld", "hello[31mworld"},
		{"drop null", "hello\x00world", "helloworld"},
		{"drop del", "hello\x7fworld", "helloworld"},
		{"CJK preserved", "你好世界", "你好世界"},
		{"emoji preserved", "hello 🎨 world", "hello 🎨 world"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			assert.Equal(t, tt.want, SanitizeText(tt.in))
		})
	}
}

func TestLinear_ImplementsObserver(t *testing.T) {
	var buf bytes.Buffer
	r := NewLinear(&buf)
	// Verify that Linear implements the Observer interface
	var _ runtimeevent.Observer = r
}

func TestLinear_FullSession(t *testing.T) {
	// Simulate a full session flow through the Linear renderer.
	var buf bytes.Buffer
	r := NewLinear(&buf)
	now := time.Now()
	sessionID := domain.NewSessionID()

	// Turn started
	turnEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 1,
		SessionID: sessionID, Kind: runtimeevent.KindTurnStarted,
		Time: now, Durable: true,
	}
	turnEvt.Payload = mustMarshal(t, runtimeevent.TurnStartedPayload{TurnIndex: 1, Prompt: "test"})
	require.NoError(t, r.ObserveDurable(turnEvt))

	// Model request started
	reqEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 2,
		SessionID: sessionID, Kind: runtimeevent.KindModelRequestStarted,
		Time: now, Durable: true,
	}
	reqEvt.Payload = mustMarshal(t, runtimeevent.ModelRequestStartedPayload{RequestID: domain.NewEventID(), Turn: 1})
	require.NoError(t, r.ObserveDurable(reqEvt))

	// Text delta
	deltaEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 3,
		SessionID: sessionID, Kind: runtimeevent.KindModelTextDelta,
		Time: now, Durable: false,
	}
	deltaEvt.Payload = mustMarshal(t, runtimeevent.ModelTextDeltaPayload{RequestID: domain.NewEventID(), Delta: "Hello from model."})
	r.ObserveEphemeral(deltaEvt)

	// Usage snapshot arrives before the completion event (as emitted by the
	// controller's stream hooks).
	usageEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 4,
		SessionID: sessionID, Kind: runtimeevent.KindUsageUpdated,
		Time: now, Durable: true,
	}
	usageEvt.Payload = mustMarshal(t, runtimeevent.UsageUpdatedPayload{InputTokens: 100, OutputTokens: 50, Turns: 1})
	require.NoError(t, r.ObserveDurable(usageEvt))

	// Model response completed
	completedEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 5,
		SessionID: sessionID, Kind: runtimeevent.KindModelResponseCompleted,
		Time: now, Durable: true,
	}
	completedEvt.Payload = mustMarshal(t, runtimeevent.ModelResponseCompletedPayload{
		RequestID: domain.NewEventID(), StopReason: domain.StopEndTurn,
	})
	require.NoError(t, r.ObserveDurable(completedEvt))

	// Run completed
	doneEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 5,
		SessionID: sessionID, Kind: runtimeevent.KindRunCompleted,
		Time: now, Durable: true,
	}
	require.NoError(t, r.ObserveDurable(doneEvt))

	output := buf.String()
	assert.Contains(t, output, "[TURN 1] test")
	assert.Contains(t, output, "[assistant]")
	assert.Contains(t, output, "Hello from model.")
	assert.Contains(t, output, "tokens: in=100 out=50")
	assert.Contains(t, output, "[completed]")
}

func mustMarshal(t *testing.T, v any) []byte {
	t.Helper()
	data, err := json.Marshal(v)
	require.NoError(t, err)
	return data
}
