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

import "regexp"

// redactPlaceholder replaces every matched secret.
const redactPlaceholder = "[REDACTED_SECRET]"

// secretPatterns is the ordered secret detector set applied to every text
// leaving the process for the extraction model and to every model output
// persisted into the memory store. Patterns are intentionally conservative
// (long, prefixed, high-entropy shapes) to bound false positives on normal
// conversation text.
var secretPatterns = []struct {
	re   *regexp.Regexp
	repl string
}{
	// PEM private key blocks (multi-line, must run before line-based rules).
	{regexp.MustCompile(`(?s)-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----.*?-----END [A-Z0-9 ]*PRIVATE KEY-----`), "[REDACTED_PRIVATE_KEY]"},
	// OpenAI-style keys: sk-..., including sk-ant- and sk-proj- variants.
	{regexp.MustCompile(`\bsk-[A-Za-z0-9_-]{16,}\b`), redactPlaceholder},
	// GitHub tokens: ghp_/gho_/ghu_/ghs_/ghr_ and fine-grained PATs.
	{regexp.MustCompile(`\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}\b`), redactPlaceholder},
	{regexp.MustCompile(`\bgithub_pat_[A-Za-z0-9_]{20,}\b`), redactPlaceholder},
	// AWS access key IDs.
	{regexp.MustCompile(`\bAKIA[0-9A-Z]{16}\b`), redactPlaceholder},
	// Slack tokens.
	{regexp.MustCompile(`\bxox[baprs]-[A-Za-z0-9-]{10,}\b`), redactPlaceholder},
	// JWTs (header.payload.signature, base64url).
	{regexp.MustCompile(`\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b`), "[REDACTED_JWT]"},
	// Authorization bearer headers; the scheme prefix is preserved.
	{regexp.MustCompile(`(?i)(bearer\s+)[A-Za-z0-9._~+/=-]{16,}`), "${1}" + redactPlaceholder},
	// Key=value / "key": "value" / key: value assignments for common
	// secret names; the key, quoting and separators are preserved.
	{regexp.MustCompile(`(?i)\b(api[_-]?key|api[_-]?secret|secret[_-]?key|access[_-]?token|auth[_-]?token|refresh[_-]?token|private[_-]?token|password|passwd)(["']?\s*[:=]\s*["']?)[A-Za-z0-9._~+/=!@#$%^*-]{8,}(["']?)`), "${1}${2}" + redactPlaceholder + "${3}"},
}

// RedactSecrets replaces recognized secret material in s with placeholders.
// It is applied to transcripts before they are uploaded to the extraction
// model and to model outputs before they are written to the memory store,
// so secrets can neither leak to the provider nor persist on disk.
func RedactSecrets(s string) string {
	for _, p := range secretPatterns {
		s = p.re.ReplaceAllString(s, p.repl)
	}
	return s
}
