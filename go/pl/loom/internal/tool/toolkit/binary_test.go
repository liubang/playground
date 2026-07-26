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
// Created: 2026/07/26

package toolkit

import (
	"strings"
	"testing"
)

func TestIsBinaryContent(t *testing.T) {
	// A CJK character ("完", 3 bytes) straddling the 8KB sample boundary:
	// the hard cut splits it mid-rune, which must NOT be judged binary.
	cjk := append([]byte(strings.Repeat("a", 8190)), []byte("完整")...)
	cjk = append(cjk, []byte(strings.Repeat("b", 100))...)
	cases := []struct {
		name string
		data []byte
		want bool
	}{
		{"plain ascii", []byte("hello world"), false},
		{"cjk straddling sample boundary", cjk, false},
		{"nul byte", append([]byte("abc"), 0x00), true},
		{"invalid utf-8 mid-sample", append([]byte(strings.Repeat("a", 100)), 0xFF, 0xFE), true},
		{"incomplete rune at short file tail", append([]byte("abc"), 0xE5), true},
		{"sample full of complete runes", []byte(strings.Repeat("字", 2730)), false},
		{"empty", []byte{}, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := IsBinaryContent(tc.data); got != tc.want {
				t.Fatalf("IsBinaryContent() = %v, want %v", got, tc.want)
			}
		})
	}
}
