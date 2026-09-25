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

package toolkit

import (
	"strings"
	"testing"
)

func TestParseSandboxPermissions(t *testing.T) {
	t.Run("absent and unset spellings mean the default sandbox", func(t *testing.T) {
		unset := []*string{nil, strPtr(""), strPtr("  "), strPtr("null"), strPtr("NULL"), strPtr(" Null ")}
		for _, raw := range unset {
			got, err := ParseSandboxPermissions(raw)
			if err != nil {
				t.Fatalf("ParseSandboxPermissions(%v) error = %v", raw, err)
			}
			if got != SandboxUseDefault {
				t.Fatalf("ParseSandboxPermissions(%v) = %q, want %q", raw, got, SandboxUseDefault)
			}
		}
	})

	t.Run("enum values pass through", func(t *testing.T) {
		for _, value := range []string{SandboxUseDefault, SandboxRequireEscalated} {
			got, err := ParseSandboxPermissions(strPtr(value))
			if err != nil || got != value {
				t.Fatalf("ParseSandboxPermissions(%q) = %q, %v; want %q, nil", value, got, err, value)
			}
		}
	})

	t.Run("invalid value echoes what was received", func(t *testing.T) {
		_, err := ParseSandboxPermissions(strPtr("escalated"))
		if err == nil {
			t.Fatal("ParseSandboxPermissions(escalated) succeeded, want error")
		}
		if !strings.Contains(err.Error(), `got "escalated"`) {
			t.Fatalf("error must echo the received value, got %q", err.Error())
		}
	})
}

func TestNormalizeJustification(t *testing.T) {
	cases := []struct {
		raw  *string
		want string
	}{
		{nil, ""},
		{strPtr(""), ""},
		{strPtr("null"), ""},
		{strPtr(" NULL "), ""},
		{strPtr(" allow deploy? "), "allow deploy?"},
	}
	for _, tc := range cases {
		if got := NormalizeJustification(tc.raw); got != tc.want {
			t.Fatalf("NormalizeJustification(%v) = %q, want %q", tc.raw, got, tc.want)
		}
	}
}

func strPtr(s string) *string { return &s }
