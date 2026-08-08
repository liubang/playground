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

package subagent

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// preparedFingerprint is the signed identity of a prepared sub-agent call:
// the HMAC binds the call ID, tool name, canonical arguments, and the
// approved risk tier, so a prepared call cannot be re-purposed between
// Prepare and Execute (REVIEW H10).
type preparedFingerprint struct {
	CallID    string           `json:"call_id"`
	ToolName  string           `json:"tool_name"`
	Arguments json.RawMessage  `json:"arguments"`
	Risk      domain.RiskLevel `json:"risk"`
}

// newSigningKey returns the random per-tool HMAC key. It never leaves the
// process, which is what elevates ArgsHash from a plain digest (anyone can
// recompute a SHA-256) to a proof that THIS tool instance prepared the
// call — the same protocol the built-in tools and the MCP adapter use.
func newSigningKey() ([32]byte, error) {
	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return [32]byte{}, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}
	return key, nil
}

func signPreparedCall(key *[32]byte, prepared domain.PreparedCall) string {
	fingerprint := preparedFingerprint{
		CallID:    prepared.Call.ID.String(),
		ToolName:  prepared.Call.Name,
		Arguments: append(json.RawMessage(nil), prepared.Call.Arguments...),
		Risk:      prepared.Risk,
	}
	payload, _ := json.Marshal(fingerprint)
	h := hmac.New(sha256.New, key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

// verifyPreparedCall re-checks a prepared call at Execute time: tool name,
// source, and risk must match the live definition, and the HMAC must
// verify against this tool instance's key.
func verifyPreparedCall(key *[32]byte, def domain.ToolDefinition, risk domain.RiskLevel, prepared domain.PreparedCall) error {
	if prepared.Call.Name != def.Name || prepared.Definition.Name != def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Source != def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call source mismatch")
	}
	if prepared.Risk != risk {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	expected := signPreparedCall(key, prepared)
	if !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}
