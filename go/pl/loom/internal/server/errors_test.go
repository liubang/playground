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
// Created: 2026/08/08

package server

import (
	"errors"
	"fmt"
	"net/http"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// TestMapErrorInvalidInputSentinel locks the structural mapping (REVIEW
// M27): typed domain.ErrInvalidInput errors map to 400 invalid_input via
// errors.As, independent of the message wording — including when wrapped.
func TestMapErrorInvalidInputSentinel(t *testing.T) {
	typed := domain.NewError(domain.ErrInvalidInput, "session ID is required")
	if se := mapError(typed); se.status != http.StatusBadRequest || se.code != "invalid_input" {
		t.Fatalf("mapError(typed) = %+v, want 400 invalid_input", se)
	}
	wrapped := fmt.Errorf("append events: %w", typed)
	if se := mapError(wrapped); se.status != http.StatusBadRequest || se.code != "invalid_input" {
		t.Fatalf("mapError(wrapped) = %+v, want 400 invalid_input", se)
	}
}

// TestMapErrorDoesNotOvermatchInvalid is the M27 regression lock: an
// internal error whose message merely contains the word "invalid" must
// stay a 500, not be mis-mapped to a client-facing 400.
func TestMapErrorDoesNotOvermatchInvalid(t *testing.T) {
	err := errors.New("model stream broke: invalid UTF-8 in provider chunk")
	if se := mapError(err); se.status != http.StatusInternalServerError || se.code != "internal" {
		t.Fatalf("mapError = %+v, want 500 internal", se)
	}
}

// TestMapErrorInvalidInputFallbackPhrases: the remaining untyped
// invalid-input producers still map to 400 via the narrowed phrase set.
func TestMapErrorInvalidInputFallbackPhrases(t *testing.T) {
	for _, msg := range []string{
		`reasoning must be off, low, medium, high, or default, got "x"`,
		`unknown model "gpt-9" (have: test/model-a)`,
		"invalid artifact reference: bad digest",
	} {
		if se := mapError(errors.New(msg)); se.status != http.StatusBadRequest || se.code != "invalid_input" {
			t.Fatalf("mapError(%q) = %+v, want 400 invalid_input", msg, se)
		}
	}
}
