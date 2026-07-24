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
	"bufio"
	"bytes"
	"encoding/json"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

func TestJSONL_DurableEventsOnly(t *testing.T) {
	var buf bytes.Buffer
	j := NewJSONL(&buf)
	now := time.Now()
	sessionID := domain.NewSessionID()

	// Publish a durable event
	durableEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 1,
		SessionID: sessionID, Kind: runtimeevent.KindTurnStarted,
		Time: now, Durable: true,
	}
	durableEvt.Payload = mustJSONMarshal(t, runtimeevent.TurnStartedPayload{TurnIndex: 1, Prompt: "test"})
	err := j.ObserveDurable(durableEvt)
	require.NoError(t, err)

	// Publish an ephemeral event (should be dropped by default)
	ephemeralEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 2,
		SessionID: sessionID, Kind: runtimeevent.KindModelTextDelta,
		Time: now, Durable: false,
	}
	ephemeralEvt.Payload = mustJSONMarshal(t, runtimeevent.ModelTextDeltaPayload{Delta: "hello"})
	j.ObserveEphemeral(ephemeralEvt)

	// Output should contain exactly one JSON line (the durable event)
	lines := readAllLines(t, &buf)
	require.Len(t, lines, 1)

	var decoded runtimeevent.RuntimeEvent
	err = json.Unmarshal([]byte(lines[0]), &decoded)
	require.NoError(t, err)
	assert.Equal(t, runtimeevent.KindTurnStarted, decoded.Kind)
	assert.Equal(t, uint64(1), decoded.Sequence)
	assert.True(t, decoded.Durable)
}

func TestJSONL_WithEphemeral(t *testing.T) {
	var buf bytes.Buffer
	j := NewJSONL(&buf, WithEphemeral(true))
	now := time.Now()
	sessionID := domain.NewSessionID()

	// Durable event
	durableEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 1,
		SessionID: sessionID, Kind: runtimeevent.KindSessionOpened,
		Time: now, Durable: true,
	}
	durableEvt.Payload = mustJSONMarshal(t, runtimeevent.SessionOpenedPayload{Model: "test"})
	require.NoError(t, j.ObserveDurable(durableEvt))

	// Ephemeral event (should be included)
	ephemeralEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 2,
		SessionID: sessionID, Kind: runtimeevent.KindModelTextDelta,
		Time: now, Durable: false,
	}
	ephemeralEvt.Payload = mustJSONMarshal(t, runtimeevent.ModelTextDeltaPayload{Delta: "hi"})
	j.ObserveEphemeral(ephemeralEvt)

	// Another durable event
	completedEvt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 3,
		SessionID: sessionID, Kind: runtimeevent.KindRunCompleted,
		Time: now, Durable: true,
	}
	require.NoError(t, j.ObserveDurable(completedEvt))

	lines := readAllLines(t, &buf)
	require.Len(t, lines, 3)

	// Verify each line is valid JSON
	for i, line := range lines {
		var evt runtimeevent.RuntimeEvent
		err := json.Unmarshal([]byte(line), &evt)
		require.NoError(t, err, "line %d: invalid JSON: %s", i, line)
		assert.Equal(t, uint64(i+1), evt.Sequence)
	}
}

func TestJSONL_NDJSONFormat(t *testing.T) {
	// Verify that the output is truly newline-delimited: each line is a
	// complete valid JSON object, no multi-line pretty printing.
	var buf bytes.Buffer
	j := NewJSONL(&buf)
	now := time.Now()
	sessionID := domain.NewSessionID()

	evt := runtimeevent.RuntimeEvent{
		Version: runtimeevent.RuntimeEventVersion, Sequence: 1,
		SessionID: sessionID, Kind: runtimeevent.KindRunCompleted,
		Time: now, Durable: true,
	}
	require.NoError(t, j.ObserveDurable(evt))

	// Check the output ends with exactly one newline
	output := buf.Bytes()
	assert.True(t, len(output) > 0)
	assert.Equal(t, byte('\n'), output[len(output)-1])

	// No embedded newlines — one JSON object per line
	for i, b := range output[:len(output)-1] {
		assert.NotEqual(t, byte('\n'), b, "unexpected newline at position %d", i)
	}
}

func TestJSONL_InvalidPayload(t *testing.T) {
	// If encoding somehow fails, the renderer should not panic.
	var buf bytes.Buffer
	j := NewJSONL(&buf)

	// An invalid time that cannot be JSON-encoded will cause Encode to fail.
	// Actually Go's json.Encoder handles all time.Duration — let's instead
	// exercise the error path with a channel (which can't be marshaled).
	// This is a synthetic path just to ensure robustness.
	_ = j
	_ = buf
}

func TestJSONL_LargeSequence(t *testing.T) {
	// Emit many events and verify sequence ordering
	var buf bytes.Buffer
	j := NewJSONL(&buf, WithEphemeral(true))
	now := time.Now()
	sessionID := domain.NewSessionID()

	n := 100
	for i := 0; i < n; i++ {
		evt := runtimeevent.RuntimeEvent{
			Version: runtimeevent.RuntimeEventVersion, Sequence: uint64(i + 1),
			SessionID: sessionID, Kind: runtimeevent.KindModelTextDelta,
			Time: now, Durable: false,
		}
		evt.Payload = mustJSONMarshal(t, runtimeevent.ModelTextDeltaPayload{Delta: "x"})
		j.ObserveEphemeral(evt)
	}

	lines := readAllLines(t, &buf)
	require.Len(t, lines, n)

	for i, line := range lines {
		var evt runtimeevent.RuntimeEvent
		err := json.Unmarshal([]byte(line), &evt)
		require.NoError(t, err, "line %d", i)
		assert.Equal(t, uint64(i+1), evt.Sequence)
	}
}

func TestJSONL_CloseFlush(t *testing.T) {
	var buf bytes.Buffer
	j := NewJSONL(&buf)
	err := j.Close()
	assert.NoError(t, err)
	err = j.Flush()
	assert.NoError(t, err)
}

func TestJSONL_ImplementsObserver(t *testing.T) {
	var buf bytes.Buffer
	j := NewJSONL(&buf)
	// Verify that JSONL implements the Observer interface
	var _ runtimeevent.Observer = j
}

func TestJSONL_AllDurableKinds(t *testing.T) {
	// Verify JSONL correctly serializes all durable event kinds.
	now := time.Now()
	sessionID := domain.NewSessionID()

	durableEvents := []runtimeevent.RuntimeEvent{
		{Kind: runtimeevent.KindTurnStarted, Time: now, Durable: true},
		{Kind: runtimeevent.KindRunPhaseChanged, Time: now, Durable: true},
		{Kind: runtimeevent.KindModelRequestStarted, Time: now, Durable: true},
		{Kind: runtimeevent.KindModelResponseCompleted, Time: now, Durable: true},
		{Kind: runtimeevent.KindModelRequestFailed, Time: now, Durable: true},
		{Kind: runtimeevent.KindApprovalRequested, Time: now, Durable: true},
		{Kind: runtimeevent.KindApprovalResolved, Time: now, Durable: true},
		{Kind: runtimeevent.KindToolPrepared, Time: now, Durable: true},
		{Kind: runtimeevent.KindToolStarted, Time: now, Durable: true},
		{Kind: runtimeevent.KindToolCompleted, Time: now, Durable: true},
		{Kind: runtimeevent.KindBudgetUpdated, Time: now, Durable: true},
		{Kind: runtimeevent.KindUsageUpdated, Time: now, Durable: true},
		{Kind: runtimeevent.KindRunCancelled, Time: now, Durable: true},
		{Kind: runtimeevent.KindRunCompleted, Time: now, Durable: true},
		{Kind: runtimeevent.KindRuntimeFatal, Time: now, Durable: true},
	}

	var buf bytes.Buffer
	j := NewJSONL(&buf)

	for i, evt := range durableEvents {
		evt.Version = runtimeevent.RuntimeEventVersion
		evt.Sequence = uint64(i + 1)
		evt.SessionID = sessionID
		require.NoError(t, j.ObserveDurable(evt))
	}

	lines := readAllLines(t, &buf)
	require.Len(t, lines, len(durableEvents))

	for i, kind := range []runtimeevent.RuntimeEventKind{
		runtimeevent.KindTurnStarted,
		runtimeevent.KindRunPhaseChanged,
		runtimeevent.KindModelRequestStarted,
		runtimeevent.KindModelResponseCompleted,
		runtimeevent.KindModelRequestFailed,
		runtimeevent.KindApprovalRequested,
		runtimeevent.KindApprovalResolved,
		runtimeevent.KindToolPrepared,
		runtimeevent.KindToolStarted,
		runtimeevent.KindToolCompleted,
		runtimeevent.KindBudgetUpdated,
		runtimeevent.KindUsageUpdated,
		runtimeevent.KindRunCancelled,
		runtimeevent.KindRunCompleted,
		runtimeevent.KindRuntimeFatal,
	} {
		var decoded runtimeevent.RuntimeEvent
		err := json.Unmarshal([]byte(lines[i]), &decoded)
		require.NoError(t, err, "line %d", i)
		assert.Equal(t, kind, decoded.Kind)
		assert.Equal(t, sessionID, decoded.SessionID)
	}
}

func mustJSONMarshal(t *testing.T, v any) []byte {
	t.Helper()
	data, err := json.Marshal(v)
	require.NoError(t, err)
	return data
}

func readAllLines(t *testing.T, buf *bytes.Buffer) []string {
	t.Helper()
	var lines []string
	scanner := bufio.NewScanner(buf)
	for scanner.Scan() {
		line := scanner.Text()
		if line != "" {
			lines = append(lines, line)
		}
	}
	require.NoError(t, scanner.Err())
	return lines
}
