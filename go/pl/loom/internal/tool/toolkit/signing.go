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
// Created: 2026/08/18

// Package toolkit holds helpers shared across loom's tool packages
// (builtin, command, skillread, ...). It exists so each tool package does
// not have to carry its own private copy of the same logic.
package toolkit

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Fingerprint is the canonical prepared-call fingerprint for HMAC signing.
// All fields are omitempty so each tool category includes only what it
// needs: FS tools include ReadPaths/WritePaths, URL tools include
// URLRequest, process tools include ExecRequest, simple tools include
// only the base fields.
type Fingerprint struct {
	CallID       string               `json:"call_id"`
	ToolName     string               `json:"tool_name"`
	Arguments    json.RawMessage      `json:"arguments"`
	ReadPaths    []string             `json:"read_paths,omitempty"`
	WritePaths   []string             `json:"write_paths,omitempty"`
	Risk         domain.RiskLevel     `json:"risk"`
	ExecRequest  *domain.ExecRequest  `json:"exec_request,omitempty"`
	URLRequest   *domain.URLRequest   `json:"url_request,omitempty"`
	WriteRequest *domain.WriteRequest `json:"write_request,omitempty"`
}

// Signer is a per-tool HMAC-SHA256 signer/verifier for prepared calls.
// It replaces the per-package baseTool.key + signPrepared + verifyPreparedCall
// triplet that was copy-pasted across every tool package.
type Signer struct {
	key [32]byte
}

// NewSigner creates a signer with a random per-instance key.
func NewSigner() (Signer, error) {
	var s Signer
	if _, err := rand.Read(s.key[:]); err != nil {
		return Signer{}, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}
	return s, nil
}

// Sign computes the HMAC-SHA256 of the prepared call's fingerprint.
func (s *Signer) Sign(prepared domain.PreparedCall) string {
	fp := Fingerprint{
		CallID:       prepared.Call.ID.String(),
		ToolName:     prepared.Call.Name,
		Arguments:    CloneRawMessage(prepared.Call.Arguments),
		ReadPaths:    append([]string(nil), prepared.ReadPaths...),
		WritePaths:   append([]string(nil), prepared.WritePaths...),
		Risk:         prepared.Risk,
		ExecRequest:  prepared.ExecRequest,
		URLRequest:   prepared.URLRequest,
		WriteRequest: prepared.WriteRequest,
	}
	return s.SignFingerprint(fp)
}

// SignFingerprint computes the HMAC-SHA256 of an arbitrary fingerprint
// struct. This is for tools whose fingerprint includes fields beyond the
// canonical Fingerprint (e.g. command's includes the full Definition).
func (s *Signer) SignFingerprint(fp any) string {
	payload, _ := json.Marshal(fp)
	return s.SignRaw(payload)
}

// SignRaw computes the HMAC-SHA256 of an arbitrary byte payload.
func (s *Signer) SignRaw(payload []byte) string {
	h := hmac.New(sha256.New, s.key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

// VerifyRaw compares two HMAC-SHA256 hex strings using constant-time
// comparison.
func (s *Signer) VerifyRaw(expected, actual string) bool {
	return hmac.Equal([]byte(expected), []byte(actual))
}

// Verify checks the prepared call against the tool definition and the
// signer's HMAC. It performs the five structural checks (name, definition,
// source, risk, capabilities) plus the HMAC comparison. Tools with
// per-argument risk re-derivation (browser, command) should do that check
// BEFORE calling Verify and skip it here.
func (s *Signer) Verify(prepared domain.PreparedCall, def domain.ToolDefinition) error {
	if prepared.Call.Name != def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Name != def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	if prepared.Definition.Source != def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call source mismatch")
	}
	if !SameCapabilities(prepared.Definition.Capabilities, def.Capabilities) {
		return domain.NewError(domain.ErrSecurity, "prepared call capabilities mismatch")
	}
	expected := s.Sign(prepared)
	if !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

// VerifyWithRisk is like Verify but also checks the risk level. Most tools
// use the definition's default risk and can call this directly. Tools that
// re-derive risk from arguments (browser, command) should skip this check
// and call Verify instead.
func (s *Signer) VerifyWithRisk(prepared domain.PreparedCall, def domain.ToolDefinition) error {
	if prepared.Risk != def.Risk() {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	return s.Verify(prepared, def)
}

// ValidateCallName checks the incoming call's name matches the tool
// definition, returning an error for a mismatch.
func ValidateCallName(call domain.ToolCall, def domain.ToolDefinition) error {
	if call.Name != def.Name {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", def.Name))
	}
	return nil
}

// DecodeStrict decodes a JSON raw message into the target type, rejecting
// unknown fields and multiple JSON values. This is the strict form used by
// all tool argument parsing.
func DecodeStrict[T any](raw json.RawMessage) (T, error) {
	var out T
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&out); err != nil {
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must be valid JSON matching the tool schema", domain.WithCause(err))
	}
	var trailing struct{}
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value")
		}
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value", domain.WithCause(err))
	}
	return out, nil
}

// CloneRawMessage returns a deep copy of a json.RawMessage.
func CloneRawMessage(raw json.RawMessage) json.RawMessage {
	if raw == nil {
		return nil
	}
	return append(json.RawMessage(nil), raw...)
}

// SameCapabilities reports whether two capability slices are equal.
func SameCapabilities(left, right []domain.Capability) bool {
	if len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
}

// SortedStrings returns a sorted copy of the input slice.
func SortedStrings(values []string) []string {
	if len(values) == 0 {
		return nil
	}
	out := append([]string(nil), values...)
	sort.Strings(out)
	return out
}

// SuccessResult builds a success ToolResult from a payload.
func SuccessResult(callID domain.ToolCallID, startedAt time.Time, payload any) domain.ToolResult {
	content, err := json.Marshal(payload)
	if err != nil {
		return ErrorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	finishedAt := time.Now()
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(content)}},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}

// ErrorResult builds an error ToolResult from an error, classifying it
// into the appropriate status/code.
func ErrorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	status := domain.ToolStatusError
	code := string(domain.ErrInternal)
	message := "internal tool error"
	retryable := false

	switch {
	case errors.Is(err, context.Canceled):
		status = domain.ToolStatusCancelled
		code = string(domain.ErrCancelled)
		message = "operation cancelled"
	case errors.Is(err, context.DeadlineExceeded):
		status = domain.ToolStatusTimeout
		code = string(domain.ErrTimeout)
		message = "operation timed out"
	default:
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) {
			code = string(agentErr.Code)
			message = agentErr.Message
			retryable = agentErr.Retryable
			switch agentErr.Code {
			case domain.ErrCancelled:
				status = domain.ToolStatusCancelled
			case domain.ErrTimeout:
				status = domain.ToolStatusTimeout
			}
		}
	}

	finishedAt := time.Now()
	return domain.ToolResult{
		CallID: callID,
		Status: status,
		Error: &domain.ToolError{
			Code:      code,
			Message:   message,
			Retryable: retryable,
		},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}
