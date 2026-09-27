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

// Semantic derivation for filesystem-destructive and privilege-escalating
// programs. The consequence follows the TARGET SHAPE, not the program
// name: rm -rf of a build directory is confined (rebuildable); rm -rf of
// a critical root, the home directory, an upward escape, or RECURSIVELY
// of anything outside the sandbox roots is local-destructive. Absolute
// targets outside the roots also carry Writes refs — deleting across
// the boundary is a boundary crossing, exactly like a write redirect,
// so an approval can widen the sandbox to the target instead of the
// command dying sandbox-blocked.
package permission

import (
	"os"
	"path/filepath"
	"strings"

	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// rmOpts is the shared option grammar of rm/rmdir/unlink (union; the
// strictest reading wins — unknown flags fail the parse).
var rmOpts = OptTable{
	Long: map[string]bool{
		"force": false, "recursive": false, "dir": false, "verbose": false,
		"interactive": false, "one-file-system": false,
		"no-preserve-root": false, "preserve-root": false,
	},
	Short: map[rune]bool{
		'r': false, 'R': false, 'f': false, 'i': false, 'I': false,
		'd': false, 'v': false,
	},
}

// semDeriveRm classifies deletion commands by their targets.
func semDeriveRm(argv []string, env DeriveEnv) (Effect, bool) {
	opts, ok := ParseOpts(argv[1:], rmOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: argv[0]}
	recursive := opts.Has("-r", "-R", "--recursive")
	for _, target := range opts.Positional {
		// Glob targets are judged by their STATIC PREFIX: the shell
		// expands them at runtime, so the analysis only ever sees the
		// literal — "~/*" must be judged as "~", not waved through as
		// an unclassifiable string.
		judge := target
		if prefix := staticGlobPrefix(target); prefix != "" {
			judge = prefix
		}
		critical := isCriticalRoot(judge)
		switch {
		case critical:
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = argv[0] + " targets a critical root (" + target + ")"
		case recursive && escapesWorkingDir(judge):
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = argv[0] + " -r escapes the working directory (" + target + ")"
		}
		abs, isAbs := absoluteRmTarget(judge)
		if !isAbs {
			continue // relative target: the sandboxed cwd confines it
		}
		clean := workspacepkg.Canonicalize(abs)
		// Deleting a credential/persistence file is as sensitive as
		// writing one. Skipped for a critical root — the destructive
		// reason says more than the write-oriented indicator would.
		if !critical {
			if reason := sensitiveRedirectTarget(clean); reason != "" {
				e.Indicators = unionStrings(e.Indicators, []string{reason})
			}
		}
		rootsKnown := len(env.Roots) > 0
		if rootsKnown && pathUnderRoots(env.Roots, clean) {
			continue // inside the sandbox's writable domain: confined
		}
		// Beyond the roots the deletion is a boundary crossing, exactly
		// like a write redirect. With unknown roots the conservative
		// reading treats only home-prefixed targets as crossings; other
		// absolute paths keep the legacy confined judgment.
		if !rootsKnown && !isHomePath(clean) {
			continue
		}
		e.Writes.Paths = unionStrings(e.Writes.Paths, []string{clean})
		if recursive && e.Consequence < ConsequenceLocalDestructive {
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = argv[0] + " -r deletes user data outside the workspace (" + target + ")"
		}
	}
	return e, true
}

// staticGlobPrefix returns the literal directory prefix of a target
// before its first glob metacharacter ("" when the target is glob-free).
func staticGlobPrefix(arg string) string {
	i := strings.IndexAny(arg, "*?[")
	if i < 0 {
		return ""
	}
	return arg[:i]
}

// absoluteRmTarget expands a ~-prefixed or absolute target for boundary
// judgment. ok=false for relative targets — the sandboxed cwd confines
// them (upward escapes are checked separately).
func absoluteRmTarget(arg string) (string, bool) {
	if arg == "~" || strings.HasPrefix(arg, "~/") {
		return expandTilde(arg), true
	}
	if filepath.IsAbs(arg) {
		return arg, true
	}
	return "", false
}

// isHomePath reports whether clean is the user's home directory or
// under it.
func isHomePath(clean string) bool {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return false
	}
	return pathUnderRoots([]string{home}, clean)
}

// chmodOpts is the shared option grammar of chmod/chown/chgrp.
var chmodOpts = OptTable{
	Long: map[string]bool{
		"recursive": false, "verbose": false, "changes": false,
		"silent": false, "quiet": false, "reference": true,
		"preserve-root": false, "no-preserve-root": false,
		"from": true, "dereference": false, "no-dereference": false,
	},
	Short: map[rune]bool{
		'R': false, 'r': false, 'v': false, 'c': false, 'f': false,
		'h': false, 'H': false, 'L': false, 'P': false,
	},
}

// semDeriveChmod classifies permission/ownership changes by their
// targets (a recursive change at a critical root is destructive).
func semDeriveChmod(argv []string, _ DeriveEnv) (Effect, bool) {
	opts, ok := ParseOpts(argv[1:], chmodOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: argv[0]}
	for _, target := range opts.Positional {
		if isCriticalRoot(target) {
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = argv[0] + " targets a critical root (" + target + ")"
			return e, true
		}
	}
	return e, true
}

// semDeriveAlwaysDestructive classifies programs that are destructive at
// any target: dd, mkfs, shred, fdisk, diskutil, newfs_*, hdiutil.
func semDeriveAlwaysDestructive(argv []string, _ DeriveEnv) (Effect, bool) {
	return Effect{
		Proven:      true,
		Consequence: ConsequenceLocalDestructive,
		Reason:      programBase(argv[0]) + " destroys data at any target",
	}, true
}

// semDerivePrivilegeEscalation classifies sudo/su/doas: they escape every
// user-level boundary, so they carry a standing indicator — an approval
// may only ever cover the exact argv, never a categorical prefix.
func semDerivePrivilegeEscalation(argv []string, _ DeriveEnv) (Effect, bool) {
	return Effect{
		Proven:      true,
		Consequence: ConsequenceLocalDestructive,
		Reason:      programBase(argv[0]) + " runs the command as another user (typically root)",
		Indicators: []string{
			programBase(argv[0]) + " escapes every user-level boundary (privilege escalation)",
		},
	}, true
}

// isCriticalRoot reports whether arg denotes /, a home directory, or a
// top-level system directory. Comparison is case-insensitive: APFS is
// case-insensitive by default, so /USERS IS /Users.
func isCriticalRoot(arg string) bool {
	arg = strings.TrimSuffix(arg, "/")
	if arg == "" || arg == "/" || arg == "~" || arg == "/*" {
		return true
	}
	if home, err := os.UserHomeDir(); err == nil && home != "" &&
		strings.EqualFold(arg, home) {
		return true
	}
	switch strings.ToLower(arg) {
	case "/bin", "/sbin", "/usr", "/etc", "/var", "/system", "/library",
		"/users", "/boot", "/opt", "/private", "/volumes", "/cores":
		return true
	}
	return false
}

// escapesWorkingDir reports whether a relative target climbs above the
// working directory (../ forms, including disguised ./../ and
// mid-path traversals like a/../../b).
func escapesWorkingDir(arg string) bool {
	if strings.HasPrefix(arg, "/") || strings.HasPrefix(arg, "~") {
		return false // absolute targets are judged by isCriticalRoot / the roots boundary
	}
	depth := 0
	for _, seg := range strings.Split(arg, "/") {
		switch seg {
		case "..":
			depth--
			if depth < 0 {
				return true
			}
		case "", ".":
		default:
			depth++
		}
	}
	return false
}
