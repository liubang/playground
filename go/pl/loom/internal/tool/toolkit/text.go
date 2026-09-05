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

import "bytes"

// SanitizeUTF8 converts arbitrary bytes to valid UTF-8, replacing each
// invalid sequence with '?'. Tool output routinely carries non-UTF-8 bytes
// (command stdout/stderr, file contents), and every byte string that crosses
// a JSON or model wire must be valid UTF-8.
func SanitizeUTF8(data []byte) string {
	return string(bytes.ToValidUTF8(data, []byte("?")))
}

// Ellipsize bounds s to max runes, appending "…" when it was cut. It is the
// single shared policy for short model- and approval-facing strings
// (approval descriptions, error echoes): an unmarked cut could be mistaken
// for the full text.
func Ellipsize(s string, max int) string {
	if max <= 0 {
		return "…"
	}
	r := []rune(s)
	if len(r) <= max {
		return s
	}
	return string(r[:max]) + "…"
}
