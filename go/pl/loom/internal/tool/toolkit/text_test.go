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
// Created: 2026/09/05

package toolkit

import "testing"

func TestSanitizeUTF8(t *testing.T) {
	if got := SanitizeUTF8([]byte{'a', 0xff, 'b'}); got != "a?b" {
		t.Fatalf("SanitizeUTF8() = %q, want a?b", got)
	}
	if got := SanitizeUTF8([]byte("你好")); got != "你好" {
		t.Fatalf("SanitizeUTF8() = %q, want 你好", got)
	}
}

func TestEllipsize(t *testing.T) {
	cases := []struct {
		in   string
		max  int
		want string
	}{
		{"hello", 3, "hel…"},
		{"hello", 10, "hello"},
		{"你好世界", 2, "你好…"},
		{"你好世界", 5, "你好世界"},
		{"hello", 0, "…"},
	}
	for _, tc := range cases {
		if got := Ellipsize(tc.in, tc.max); got != tc.want {
			t.Errorf("Ellipsize(%q, %d) = %q, want %q", tc.in, tc.max, got, tc.want)
		}
	}
}
