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

// Package replay implements LLM call recording and keyless replay at the
// domain.Model boundary (docs/REPLAY_TESTING_DESIGN.md). A RecordingModel
// wraps a real provider and persists every ModelEvent stream as a
// calls.jsonl fixture; a ReplayModel serves the recorded streams back
// positionally, so a recorded agent loop can be re-run deterministically
// without a provider key.
package replay

import (
	"context"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// RootBindingKey is the binding key of the top-level (non-delegated)
// session. Delegated (sub-agent) sessions bind by their parent tool call
// ID, which originates from the recorded model stream and is therefore
// stable across record and replay runs even under parallel delegations
// (REPLAY_TESTING_DESIGN §3.3).
const RootBindingKey = "root"

type sessionRefContextKey struct{}

// SessionRef identifies which session a model call belongs to. It rides
// the request context from Loop.Execute down to the Model implementation;
// only the record/replay test infrastructure consumes it.
type SessionRef struct {
	SessionID        domain.SessionID
	ParentToolCallID domain.ToolCallID
}

// WithSessionRef returns a context carrying the session identity of the
// loop about to run. The root loop passes a zero parentToolCallID;
// sub-agent loops pass the delegating tool call's ID.
func WithSessionRef(ctx context.Context, sessionID domain.SessionID, parentToolCallID domain.ToolCallID) context.Context {
	return context.WithValue(ctx, sessionRefContextKey{}, SessionRef{
		SessionID:        sessionID,
		ParentToolCallID: parentToolCallID,
	})
}

// SessionRefFrom extracts the session identity from a context.
func SessionRefFrom(ctx context.Context) (SessionRef, bool) {
	if ctx == nil {
		return SessionRef{}, false
	}
	ref, ok := ctx.Value(sessionRefContextKey{}).(SessionRef)
	return ref, ok
}

// BindingKeyFrom returns the fixture binding key for a model call issued
// under ctx: RootBindingKey for the top-level session, the parent tool
// call ID for a delegated session.
func BindingKeyFrom(ctx context.Context) string {
	ref, ok := SessionRefFrom(ctx)
	if !ok || ref.ParentToolCallID.String() == "" {
		return RootBindingKey
	}
	return ref.ParentToolCallID.String()
}
