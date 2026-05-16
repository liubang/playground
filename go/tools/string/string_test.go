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
// Created: 2026/05/16 20:56

package string

import "testing"

func TestStrPrintLenInTerm(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected uint64
	}{
		{
			name:     "empty string",
			input:    "",
			expected: 0,
		},
		{
			name:     "ascii only",
			input:    "hello",
			expected: 5,
		},
		{
			name:     "ascii with spaces",
			input:    "hello world",
			expected: 11,
		},
		{
			name:     "chinese characters",
			input:    "你好",
			expected: 4,
		},
		{
			name:     "mixed ascii and chinese",
			input:    "hello你好",
			expected: 9,
		},
		{
			name:     "japanese characters",
			input:    "こんにちは",
			expected: 10,
		},
		{
			name:     "korean characters",
			input:    "안녕하세요",
			expected: 10,
		},
		{
			name:     "emoji",
			input:    "👋",
			expected: 2,
		},
		{
			name:     "combined emoji",
			input:    "👨‍👩‍👧‍👦",
			expected: 2,
		},
		{
			name:     "single ascii char",
			input:    "a",
			expected: 1,
		},
		{
			name:     "tab character",
			input:    "\t",
			expected: 0,
		},
		{
			name:     "mixed with numbers",
			input:    "abc123",
			expected: 6,
		},
		{
			name:     "fullwidth latin",
			input:    "ＡＢＣ",
			expected: 6,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := StrPrintLenInTerm(tt.input)
			if got != tt.expected {
				t.Errorf("StrPrintLenInTerm(%q) = %d, want %d", tt.input, got, tt.expected)
			}
		})
	}
}
