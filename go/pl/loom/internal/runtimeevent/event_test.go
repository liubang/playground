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

package runtimeevent

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestRuntimeEventValidate(t *testing.T) {
	valid := RuntimeEvent{
		Version:   RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      KindTurnStarted,
		Time:      time.Now(),
		Durable:   true,
	}
	if err := valid.Validate(); err != nil {
		t.Fatalf("valid event: %v", err)
	}
}

func TestRuntimeEventValidateRejectsInvalidVersion(t *testing.T) {
	evt := RuntimeEvent{
		Version:   999,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      KindTurnStarted,
	}
	if err := evt.Validate(); err == nil {
		t.Fatal("expected error for invalid version")
	}
}

func TestRuntimeEventValidateRejectsZeroSequence(t *testing.T) {
	evt := RuntimeEvent{
		Version:   RuntimeEventVersion,
		Sequence:  0,
		SessionID: domain.NewSessionID(),
		Kind:      KindTurnStarted,
	}
	if err := evt.Validate(); err == nil {
		t.Fatal("expected error for zero sequence")
	}
}

func TestRuntimeEventValidateRejectsEmptySessionID(t *testing.T) {
	evt := RuntimeEvent{
		Version:  RuntimeEventVersion,
		Sequence: 1,
		Kind:     KindTurnStarted,
	}
	if err := evt.Validate(); err == nil {
		t.Fatal("expected error for empty session ID")
	}
}

func TestRuntimeEventValidateRejectsUnknownKind(t *testing.T) {
	evt := RuntimeEvent{
		Version:   RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      RuntimeEventKind("unknown.kind"),
	}
	if err := evt.Validate(); err == nil {
		t.Fatal("expected error for unknown kind")
	}
}

func TestRuntimeEventValidateRejectsInvalidPayloadJSON(t *testing.T) {
	evt := RuntimeEvent{
		Version:   RuntimeEventVersion,
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Kind:      KindTurnStarted,
		Payload:   json.RawMessage("{invalid"),
	}
	if err := evt.Validate(); err == nil {
		t.Fatal("expected error for invalid payload JSON")
	}
}

func TestAllRuntimeEventKindsValidate(t *testing.T) {
	sessionID := domain.NewSessionID()
	kinds := []RuntimeEventKind{
		KindSessionOpened, KindSessionClosed,
		KindTurnStarted, KindTurnFinished,
		KindRunPhaseChanged,
		KindModelRequestStarted, KindModelTextDelta, KindModelReasoningDelta, KindModelToolCallDelta,
		KindModelResponseCompleted, KindModelRequestFailed,
		KindApprovalRequested, KindApprovalResolved,
		KindToolPrepared, KindToolStarted, KindToolCompleted, KindToolProgress,
		KindBudgetUpdated, KindUsageUpdated,
		KindRunCancelRequested, KindRunCancelled, KindRunCompleted,
		KindRuntimeWarning, KindRuntimeFatal,
	}
	for _, kind := range kinds {
		evt := RuntimeEvent{
			Version:   RuntimeEventVersion,
			Sequence:  1,
			SessionID: sessionID,
			Kind:      kind,
		}
		if err := evt.Validate(); err != nil {
			t.Errorf("kind %q: %v", kind, err)
		}
	}
}

func TestSessionOpenedPayloadRoundTrip(t *testing.T) {
	payload := SessionOpenedPayload{
		Model:     "gpt-4",
		Workspace: "/home/user/project",
		Resumed:   true,
	}
	data, err := json.Marshal(payload)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var decoded SessionOpenedPayload
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if decoded.Model != payload.Model || decoded.Workspace != payload.Workspace || decoded.Resumed != payload.Resumed {
		t.Fatalf("round-trip mismatch: %+v vs %+v", decoded, payload)
	}
}
