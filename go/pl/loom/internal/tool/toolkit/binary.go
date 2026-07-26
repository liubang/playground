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

// Package toolkit holds helpers shared across loom's tool packages
// (builtin, command, skillread, ...). It exists so each tool package does
// not have to carry its own private copy of the same logic; the prepared
// call signing protocol, strict JSON decoding, and result helpers are
// candidates for future consolidation here.
package toolkit

import (
	"bytes"
	"unicode/utf8"
)

// BinarySampleBytes is the sample window for binary content detection.
const BinarySampleBytes = 8 << 10

// IsBinaryContent reports whether data looks like binary (or non-UTF-8)
// content: a NUL byte in the sample, or invalid UTF-8. A hard sample cut
// can split a multi-byte UTF-8 character at the boundary: only then, drop a
// trailing incomplete rune prefix (up to 3 bytes) so plain text is never
// misclassified as binary. Genuinely invalid bytes (0xFF, lone
// continuation, overlong lead) are not a valid rune prefix and keep the
// sample classified as binary.
func IsBinaryContent(data []byte) bool {
	sample := data
	truncated := len(sample) >= BinarySampleBytes
	if truncated {
		sample = sample[:BinarySampleBytes]
	}
	if truncated && !utf8.Valid(sample) {
		sample = trimIncompleteRuneSuffix(sample)
	}
	return bytes.IndexByte(sample, 0) >= 0 || !utf8.Valid(sample)
}

// trimIncompleteRuneSuffix drops a trailing incomplete UTF-8 rune prefix
// (lead byte followed by continuation bytes) left by a hard cut. If the
// sample does not end with such a prefix, it is returned unchanged.
func trimIncompleteRuneSuffix(sample []byte) []byte {
	// Walk back over continuation bytes (10xxxxxx) to the rune start.
	i := len(sample) - 1
	for i >= 0 && sample[i]&0xC0 == 0x80 {
		i--
	}
	if i < 0 {
		return sample
	}
	lead := sample[i]
	var size int
	switch {
	case lead&0x80 == 0:
		return sample // ASCII: complete rune, nothing to trim
	case lead&0xE0 == 0xC0:
		size = 2
	case lead&0xF0 == 0xE0:
		size = 3
	case lead&0xF8 == 0xF0:
		size = 4
	default:
		return sample // invalid lead byte: not a rune prefix
	}
	if len(sample)-i < size {
		return sample[:i] // incomplete rune at the cut point: drop it
	}
	return sample
}
