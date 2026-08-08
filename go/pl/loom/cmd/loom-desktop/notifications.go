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

// Notification Center integration: agent milestones that need the user's
// attention (approval requests, model questions) or mark the end of a long
// wait (turn finished/failed) are mirrored to macOS notifications, so the
// user can switch away while Loom works.
//
// v1 notifies unconditionally: distinguishing "user is already watching"
// from "user is elsewhere" requires NSApplication.isActive, which is only
// safe on the main thread. The banner volume is low (one per turn /
// approval), so the false-positive cost is a transient banner.

package main

import (
	"context"
	"encoding/json"
	"log/slog"

	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// notifyFunc delivers one user-visible notification. systemNotify is the
// production implementation; tests substitute a capture function.
var notifyFunc = systemNotify

// watchNotifications subscribes to the runtime event broker and forwards
// attention-worthy events to notifyFunc until ctx ends or the broker
// closes.
func watchNotifications(ctx context.Context, broker *runtimeevent.Broker, logger *slog.Logger) {
	events, unsubscribe := broker.Subscribe()
	defer unsubscribe()
	for {
		select {
		case <-ctx.Done():
			return
		case evt, ok := <-events:
			if !ok {
				return
			}
			if err := notifyForEvent(evt); err != nil {
				logger.Debug("notification payload", "kind", evt.Kind, "error", err)
			}
		}
	}
}

func notifyForEvent(evt runtimeevent.RuntimeEvent) error {
	switch evt.Kind {
	case runtimeevent.KindApprovalRequested:
		var p runtimeevent.ApprovalRequestedPayload
		if err := json.Unmarshal(evt.Payload, &p); err != nil {
			return err
		}
		body := "A tool call is waiting for your decision."
		if p.ToolName != "" {
			body = p.ToolName + ": " + truncateRunes(p.Description, 120)
		}
		notifyFunc("Approval needed", body)
	case runtimeevent.KindQuestionAsked:
		var p runtimeevent.QuestionAskedPayload
		if err := json.Unmarshal(evt.Payload, &p); err != nil {
			return err
		}
		body := "The model asked a question mid-run."
		if p.Text != "" {
			body = truncateRunes(p.Text, 120)
		}
		notifyFunc("Loom has a question", body)
	case runtimeevent.KindTurnFinished:
		var p runtimeevent.TurnFinishedPayload
		if err := json.Unmarshal(evt.Payload, &p); err != nil {
			return err
		}
		if p.Error != "" {
			notifyFunc("Turn failed", truncateRunes(p.Error, 120))
		} else {
			notifyFunc("Turn finished", "Loom finished the current turn.")
		}
	}
	return nil
}

func truncateRunes(s string, max int) string {
	r := []rune(s)
	if len(r) <= max {
		return s
	}
	return string(r[:max-1]) + "…"
}
