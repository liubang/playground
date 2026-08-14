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
// Created: 2026/08/14

package render

import (
	"regexp"
	"strings"
)

// displaySafeArg matches argv elements that need no quoting: a plain join
// renders them exactly as the shell would read them back. Anything else
// (whitespace, metacharacters, quotes, empty string) is single-quoted so the
// displayed command line stays unambiguous — "sh -c 'echo a; echo b'" must
// not render as "sh -c echo a; echo b".
var displaySafeArg = regexp.MustCompile(`^[A-Za-z0-9_@%+=:,./-]+$`)

// CommandLineForDisplay joins argv into a one-line display form, quoting
// elements that would otherwise blend into their neighbors. Display-only;
// the WebUI snapshot path mirrors this exact rule in JS (blocks.js
// histTarget) so a block reads the same before and after a session switch.
func CommandLineForDisplay(argv []string) string {
	if len(argv) == 0 {
		return ""
	}
	quoted := make([]string, 0, len(argv))
	for _, arg := range argv {
		quoted = append(quoted, quoteArgForDisplay(arg))
	}
	return strings.Join(quoted, " ")
}

// quoteArgForDisplay single-quotes an argv element when a plain rendering
// would be ambiguous, using the POSIX '"'"' escape for embedded quotes.
func quoteArgForDisplay(arg string) string {
	if displaySafeArg.MatchString(arg) {
		return arg
	}
	return "'" + strings.ReplaceAll(arg, "'", `'"'"'`) + "'"
}
