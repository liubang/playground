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

// getopt normalization for the semantic command tables. Each command
// family declares its OWN option grammar (which flags take values),
// and ParseOpts normalizes an argument vector against it: combined
// short flags are split (-fu → -f -u), --flag=value is reduced to the
// flag, unambiguous long-option abbreviations are resolved against the
// table (GNU getopt_long accepts them — rm --rec IS --recursive), and
// the -- terminator is honored. Anything the table cannot explain makes
// the parse fail, and the caller degrades to "unprovable" — never to
// "safe". Parsing is whitelist-shaped by construction.
package permission

import (
	"strings"
)

// OptTable is one program's option grammar. Long maps a long option name
// (without "--") to whether it takes a value; Short maps a short option
// letter to the same.
type OptTable struct {
	Long  map[string]bool
	Short map[rune]bool
}

// Opts is the normalized result: canonical flag tokens (long options
// keep their full spelling; short options are split one per token,
// values dropped) and the positional arguments in order.
type Opts struct {
	Flags      []string
	Positional []string
}

// Has reports whether a flag was present (canonical long spelling or
// any of the given short letters as "-x").
func (o Opts) Has(names ...string) bool {
	for _, f := range o.Flags {
		for _, n := range names {
			if f == n {
				return true
			}
		}
	}
	return false
}

// ParseOpts normalizes args per the table. ok=false on an unknown or
// ambiguous option — the table could not fully explain the invocation.
func ParseOpts(args []string, t OptTable) (Opts, bool) {
	var out Opts
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--":
			out.Positional = append(out.Positional, args[i+1:]...)
			return out, true
		case strings.HasPrefix(a, "--"):
			name, _, hasValue := strings.Cut(a[2:], "=")
			canonical, takesValue, ok := resolveLong(name, t)
			if !ok {
				return Opts{}, false
			}
			out.Flags = append(out.Flags, "--"+canonical)
			if takesValue && !hasValue {
				i++ // value is the next token
			}
		case len(a) > 1 && a[0] == '-':
			letters := []rune(a[1:])
			for j := 0; j < len(letters); j++ {
				takesValue, known := t.Short[letters[j]]
				if !known {
					return Opts{}, false
				}
				out.Flags = append(out.Flags, "-"+string(letters[j]))
				if takesValue {
					if j+1 == len(letters) {
						i++ // value is the next token
					}
					// Otherwise the rest of the token is the
					// attached value (-n5) — stop scanning.
					break
				}
			}
		default:
			out.Positional = append(out.Positional, a)
		}
	}
	return out, true
}

// resolveLong resolves a long option name, accepting an unambiguous
// prefix abbreviation (GNU getopt_long semantics). ok=false when the
// name is unknown or the abbreviation is ambiguous.
func resolveLong(name string, t OptTable) (canonical string, takesValue, ok bool) {
	if v, hit := t.Long[name]; hit {
		return name, v, true
	}
	var matches []string
	for opt := range t.Long {
		if strings.HasPrefix(opt, name) {
			matches = append(matches, opt)
		}
	}
	if len(matches) != 1 {
		return "", false, false
	}
	return matches[0], t.Long[matches[0]], true
}
