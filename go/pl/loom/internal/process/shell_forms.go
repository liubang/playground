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
// Created: 2026/08/23

package process

import "strings"

// ShellScriptForm extracts the inline script from a shell -c invocation,
// understanding the option grammar instead of hard-matching
// ["sh", "-c", script]:
//
//   - the c flag inside any combined short-option token: sh -c, bash -lc,
//     sh -ec, zsh -fc;
//   - an attached script word: sh -cSCRIPT;
//   - trailing arg0/args after the script: sh -c SCRIPT NAME [ARGS...];
//   - value-taking options consumed correctly (-o OPTION, -O SHOPT).
//
// ok=false covers every other shell invocation shape (script file, bare
// stdin, --, unrecognized flag layouts): callers must treat !ok as
// "unanalyzable shell invocation", never as "safe".
func ShellScriptForm(argv []string) (script string, ok bool) {
	if len(argv) < 2 || !IsShellProgram(argv[0]) {
		return "", false
	}
	args := argv[1:]
	for i := 0; i < len(args); i++ {
		token := args[i]
		if token == "--" || token == "-" || token == "" || token[0] != '-' {
			// A positional (or the options terminator) before any -c:
			// this is a script-file or bare invocation, not a -c form.
			return "", false
		}
		if strings.HasPrefix(token, "--") {
			// Shell long options (--norc, --noprofile, --restricted)
			// are boolean; keep scanning.
			continue
		}
		if script, consumed, ok := scanShortOptions(token[1:], args, i); ok {
			return script, true
		} else if consumed > 0 {
			i += consumed
		}
	}
	return "", false
}

// scanShortOptions walks one combined short-option token letter by
// letter. When it finds the c flag it returns the script: the remainder
// of the token if present (sh -cSCRIPT), otherwise the next argv token
// (which may leave arg0/args behind — callers get the script only).
// consumed reports how many FOLLOWING argv tokens were swallowed by
// value-taking options (-o/-O) when no c flag was found, so the caller
// can skip them; ok=false means this token carried no -c.
func scanShortOptions(letters string, args []string, i int) (script string, consumed int, ok bool) {
	for j := 0; j < len(letters); j++ {
		switch letters[j] {
		case 'c':
			if rest := letters[j+1:]; rest != "" {
				return rest, 0, true
			}
			if i+1 < len(args) {
				return args[i+1], 0, true
			}
			return "", 0, false
		case 'o', 'O':
			// Value-taking: the value is the rest of this token, or
			// the next argv token.
			if letters[j+1:] != "" {
				return "", 0, false
			}
			return "", 1, false
		}
	}
	return "", 0, false
}
