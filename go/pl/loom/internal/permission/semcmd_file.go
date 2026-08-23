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
// a critical root, the home directory, or an upward escape is
// local-destructive.
package permission

import (
	"os"
	"strings"
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
func semDeriveRm(argv []string) (Effect, bool) {
	opts, ok := ParseOpts(argv[1:], rmOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: argv[0]}
	recursive := opts.Has("-r", "-R", "--recursive")
	for _, target := range opts.Positional {
		if isCriticalRoot(target) {
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = argv[0] + " targets a critical root (" + target + ")"
			return e, true
		}
		if recursive && escapesWorkingDir(target) {
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = argv[0] + " -r escapes the working directory (" + target + ")"
			return e, true
		}
	}
	return e, true
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
func semDeriveChmod(argv []string) (Effect, bool) {
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
func semDeriveAlwaysDestructive(argv []string) (Effect, bool) {
	return Effect{
		Proven:      true,
		Consequence: ConsequenceLocalDestructive,
		Reason:      programBase(argv[0]) + " destroys data at any target",
	}, true
}

// semDerivePrivilegeEscalation classifies sudo/su/doas: they escape every
// user-level boundary, so they carry a standing indicator — an approval
// may only ever cover the exact argv, never a categorical prefix.
func semDerivePrivilegeEscalation(argv []string) (Effect, bool) {
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
		return false // absolute targets are judged by isCriticalRoot
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
