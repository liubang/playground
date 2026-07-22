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
// Created: 2026/07/22 21:10

package domain

import (
	"errors"
	"fmt"
	"testing"
)

func TestAgentErrorCode(t *testing.T) {
	err := NewError(ErrPermission, "access denied", WithRetryable(false))
	if err.Code != ErrPermission {
		t.Errorf("expected %s, got %s", ErrPermission, err.Code)
	}
	if err.Error() != "[permission] access denied" {
		t.Errorf("unexpected error string: %s", err.Error())
	}
}

func TestAgentErrorWithCause(t *testing.T) {
	cause := errors.New("root cause")
	err := NewError(ErrUnavailable, "service down", WithCause(cause))
	if err.Unwrap() != cause {
		t.Error("expected cause to be unwrapped")
	}
	if !errors.Is(err, cause) {
		t.Error("errors.Is should match cause")
	}
}

func TestAgentErrorRetryable(t *testing.T) {
	err := NewError(ErrTimeout, "timed out", WithRetryable(true))
	if !err.Retryable {
		t.Error("expected retryable")
	}
	if !IsRetryable(err) {
		t.Error("IsRetryable should return true")
	}
}

func TestAgentErrorNotRetryable(t *testing.T) {
	err := NewError(ErrPermission, "denied")
	if IsRetryable(err) {
		t.Error("expected not retryable")
	}
}

func TestIsRetryableNonAgentError(t *testing.T) {
	if IsRetryable(errors.New("plain error")) {
		t.Error("plain error should not be retryable")
	}
}

func TestAllErrorCodes(t *testing.T) {
	codes := []ErrorCode{
		ErrInvalidInput, ErrPermission, ErrConflict, ErrUnavailable,
		ErrRateLimited, ErrTimeout, ErrCancelled, ErrBudget,
		ErrSecurity, ErrInternal,
	}
	for _, code := range codes {
		err := NewError(code, fmt.Sprintf("test %s", code))
		if err.Code != code {
			t.Errorf("expected code %s, got %s", code, err.Code)
		}
	}
}

func TestAsAgentError(t *testing.T) {
	inner := NewError(ErrTimeout, "timeout", WithRetryable(true))
	outer := fmt.Errorf("wrapped: %w", inner)

	var ae *AgentError
	if !As(outer, &ae) {
		t.Fatal("expected As to find AgentError")
	}
	if ae.Code != ErrTimeout {
		t.Errorf("expected code %s, got %s", ErrTimeout, ae.Code)
	}
}
