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
// Created: 2026/09/30

// Semantic derivation for read-only file viewers (cat/head/tail/nl/od).
// These programs only read files into stdout — proven-confined by
// construction — but a positional argument naming a credential path means
// secret material is about to enter the transcript. The seatbelt sandbox
// already hard-denies reads of the home credential locations
// (workspace/sensitive.go); the indicator exists for the paths the sandbox
// cannot deny without breaking legitimate workloads (a workspace .env a
// dev server must load), so the ATTEMPT is surfaced for approval exactly
// like a write into a credential path is (indicators.go).
package permission

import (
	"path/filepath"
	"strings"

	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// fileReadOpts maps each viewer to its option grammar. No flag of these
// programs takes a file path as its value, so a value-consuming flag
// never hides a target from the positional scan. Unknown flags fail the
// parse — the invocation degrades to unprovable, never to "safe".
var fileReadOpts = map[string]OptTable{
	"cat": {
		Long: map[string]bool{
			"number": false, "number-nonblank": false, "squeeze-blank": false,
			"show-ends": false, "show-tabs": false, "show-nonprinting": false,
			"show-all": false,
		},
		Short: map[rune]bool{
			'n': false, 'b': false, 's': false, 'e': false, 't': false,
			'u': false, 'v': false, 'A': false, 'E': false, 'T': false,
		},
	},
	"head": {
		Long: map[string]bool{
			"lines": true, "bytes": true, "quiet": false, "silent": false,
			"verbose": false, "zero-terminated": false,
		},
		Short: map[rune]bool{
			'n': true, 'c': true, 'q': false, 'v': false, 'z': false,
		},
	},
	"tail": {
		Long: map[string]bool{
			"lines": true, "bytes": true, "follow": false, "pid": true,
			"quiet": false, "silent": false, "verbose": false, "retry": false,
			"sleep-interval": true, "zero-terminated": false,
			"max-unchanged-stats": true,
		},
		Short: map[rune]bool{
			'n': true, 'c': true, 's': true, 'f': false, 'F': false,
			'q': false, 'v': false, 'z': false,
		},
	},
	"nl": {
		Long: map[string]bool{
			"body-numbering": true, "header-numbering": true,
			"footer-numbering": true, "line-increment": true,
			"join-blank-lines": true, "number-format": true,
			"number-width": true, "number-separator": true,
			"section-delimiter": true, "starting-line-number": true,
			"no-renumber": false,
		},
		Short: map[rune]bool{
			'b': true, 'f': true, 'h': true, 'i': true, 'l': true,
			'n': true, 's': true, 'v': true, 'w': true, 'd': true,
			'p': false,
		},
	},
	"od": {
		Long: map[string]bool{
			"address-radix": true, "endian": true, "format": true,
			"skip-bytes": true, "read-bytes": true, "strings": true,
			"traditional": false, "width": true,
		},
		Short: map[rune]bool{
			'A': true, 'j': true, 'N': true, 't': true, 'w': true,
			's': true, 'v': false,
			// Traditional single-character format shorthands.
			'b': false, 'c': false, 'd': false, 'o': false, 'x': false,
		},
	},
}

// semDeriveFileRead classifies a read-only viewer: confined, with a
// credential-path indicator when any positional target names sensitive
// material (workspace/sensitive.go is the single source of truth; the
// ".git" component is exempt — repository metadata reads are ordinary
// exploration, and gitMetaWrite already guards the write side).
func semDeriveFileRead(argv []string, _ DeriveEnv) (Effect, bool) {
	base := programBase(argv[0])
	table, ok := fileReadOpts[base]
	if !ok {
		return Effect{}, false
	}
	opts, ok := ParseOpts(argv[1:], table)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: base + " reads local files"}
	for _, target := range opts.Positional {
		if reason := sensitiveReadTarget(target); reason != "" {
			e.Indicators = unionStrings(e.Indicators, []string{base + " " + reason})
		}
	}
	return e, true
}

// sensitiveReadTarget returns the indicator body when a viewer's target
// names credential material — the read-side mirror of
// sensitiveRedirectTarget. The component scan runs on both relative and
// absolute forms with ".git" exempt (repository metadata reads are
// ordinary exploration; gitMetaWrite guards the write side), and
// home-rooted credential locations (~/.aws, ~/.netrc, keychains) are
// covered by workspace.IsSensitiveHomeLocation — workspace/sensitive.go
// stays the single source of truth for what counts as sensitive.
func sensitiveReadTarget(target string) string {
	expanded := expandTilde(target)
	for _, seg := range strings.Split(filepath.ToSlash(filepath.Clean(expanded)), "/") {
		if seg == ".git" {
			continue
		}
		if workspacepkg.IsSensitive(seg) {
			return "reads a credential path (" + target + ") — secret material must not enter the transcript"
		}
	}
	if workspacepkg.IsSensitiveHomeLocation(workspacepkg.Canonicalize(expanded)) {
		return "reads a credential path (" + target + ") — secret material must not enter the transcript"
	}
	return ""
}
