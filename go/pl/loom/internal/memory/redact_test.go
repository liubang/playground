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
// Created: 2026/08/06

package memory

import (
	"strings"
	"testing"
)

func TestRedactSecrets(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		wantGone []string
		wantKeep []string
	}{
		{
			name:     "openai key",
			input:    `use api key sk-abcdefghijklmnopqrstuvwxyz1234567890 for requests`,
			wantGone: []string{"sk-abcdefghijklmnopqrstuvwxyz1234567890"},
			wantKeep: []string{"[REDACTED_SECRET]"},
		},
		{
			name:     "github pat",
			input:    `token: ghp_abcdefghijklmnopqrstuvwxyz123456`,
			wantGone: []string{"ghp_abcdefghijklmnopqrstuvwxyz123456"},
		},
		{
			name:     "aws access key",
			input:    `AKIAIOSFODNN7EXAMPLE in the config`,
			wantGone: []string{"AKIAIOSFODNN7EXAMPLE"},
		},
		{
			name:     "bearer header keeps scheme",
			input:    `Authorization: Bearer abcdef1234567890abcdef1234567890`,
			wantGone: []string{"abcdef1234567890abcdef1234567890"},
			wantKeep: []string{"Bearer [REDACTED_SECRET]"},
		},
		{
			name:     "key=value assignment keeps key name",
			input:    `api_key=22063956300186390573abcdef`,
			wantGone: []string{"22063956300186390573abcdef"},
			wantKeep: []string{"api_key="},
		},
		{
			name:     "json style assignment",
			input:    `{"password": "hunter2hunter2"}`,
			wantGone: []string{"hunter2hunter2"},
			wantKeep: []string{"password"},
		},
		{
			name:     "jwt",
			input:    `token eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c`,
			wantGone: []string{"eyJhbGciOiJIUzI1NiJ9"},
		},
		{
			name:     "pem block",
			input:    "before\n-----BEGIN RSA PRIVATE KEY-----\nMIIEpAIBAAKCAQEA7\n-----END RSA PRIVATE KEY-----\nafter",
			wantGone: []string{"MIIEpAIBAAKCAQEA7"},
			wantKeep: []string{"before", "after", "[REDACTED_PRIVATE_KEY]"},
		},
		{
			name:     "ordinary text untouched",
			input:    `the user prefers table-driven tests and 4-space indent`,
			wantGone: nil,
			wantKeep: []string{"table-driven tests", "4-space indent"},
		},
		{
			name:     "short values are not secrets",
			input:    `set token=abc123 and move on`,
			wantGone: nil,
			wantKeep: []string{"token=abc123"},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := RedactSecrets(tt.input)
			for _, gone := range tt.wantGone {
				if strings.Contains(got, gone) {
					t.Errorf("RedactSecrets(%q) still contains %q", tt.input, gone)
				}
			}
			for _, keep := range tt.wantKeep {
				if !strings.Contains(got, keep) {
					t.Errorf("RedactSecrets(%q) = %q, want it to keep %q", tt.input, got, keep)
				}
			}
		})
	}
}
