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
// Created: 2026/07/31

package process

import (
	"regexp"
	"strings"
)

// shellCompositionChars are the characters that give a shell script its
// compositional power — pipes, redirections, sequencing, substitution and
// escapes. A script containing any of them can denote behavior a plain
// argv cannot express, so it must keep shell-level (R3) treatment.
const shellCompositionChars = "|&;<>()$`\\\n\r"

// shellAssignPrefix matches leading VAR=value tokens: a script starting
// with an environment assignment must not be unwrapped, because the first
// token is not the program.
var shellAssignPrefix = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*=`)

// UnwrapSimpleShell rewrites ["sh", "-c", script] (or another supported
// shell) into the plain argv the script denotes — but only when the script
// provably contains no shell composition: no pipes, redirections,
// sequencing, command/variable substitution, escapes, or leading env
// assignments. Globs are kept verbatim: they may expand at execution time,
// but they cannot introduce a different program, so classification on the
// unwrapped argv stays sound (the sandbox remains the execution boundary).
//
// This is NOT a shell parser: anything it cannot prove simple is rejected
// (ok=false) and keeps shell-level treatment. Execution is unaffected —
// the command still runs through the shell; the unwrapped argv only feeds
// risk classification, danger screening, and argv-prefix rule matching.
func UnwrapSimpleShell(argv []string) ([]string, bool) {
	if len(argv) != 3 || !IsShellProgram(argv[0]) || argv[1] != "-c" {
		return nil, false
	}
	script := argv[2]
	if strings.ContainsAny(script, shellCompositionChars) {
		return nil, false
	}
	tokens, ok := splitSimpleShellWords(script)
	if !ok || len(tokens) == 0 {
		return nil, false
	}
	if shellAssignPrefix.MatchString(tokens[0]) {
		return nil, false
	}
	return tokens, true
}

// splitSimpleShellWords splits a script on whitespace honoring single and
// double quotes. With composition characters already rejected, both quote
// forms are literal; an unterminated quote fails the split.
func splitSimpleShellWords(script string) ([]string, bool) {
	var tokens []string
	var current strings.Builder
	var quote rune // 0 = unquoted, '\'' or '"'
	flush := func() {
		if current.Len() > 0 {
			tokens = append(tokens, current.String())
			current.Reset()
		}
	}
	for _, r := range script {
		switch {
		case quote != 0:
			if r == quote {
				quote = 0
			} else {
				current.WriteRune(r)
			}
		case r == '\'' || r == '"':
			quote = r
		case r == ' ' || r == '\t' || r == '\f' || r == '\v':
			flush()
		default:
			current.WriteRune(r)
		}
	}
	if quote != 0 {
		return nil, false
	}
	flush()
	for _, tok := range tokens {
		if tok == "" {
			// Adjacent quotes ('' or "") produce empty words; they never
			// change the program but complicate prefix matching — reject.
			return nil, false
		}
	}
	return tokens, true
}
