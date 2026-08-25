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

// argv-level helpers shared by package binding and memory derivation:
// exact-token prefix matching, trusted-directory basename normalization,
// and the categorical prefix a call may be remembered by.
package permission

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// ArgvHasPrefix reports whether argv starts with prefix (exact tokens).
func ArgvHasPrefix(argv, prefix []string) bool {
	if len(prefix) == 0 || len(argv) < len(prefix) {
		return false
	}
	for i := range prefix {
		if argv[i] != prefix[i] {
			return false
		}
	}
	return true
}

// stringSliceEqual compares two token slices exactly.
func stringSliceEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// trustedProgramDirs are candidate system directories whose executables
// may resolve through basename bindings (/bin/ls → ls) — but only after
// a runtime check proves the directory is trustworthy
// (isTrustedProgramDir). Anything else must match bindings by full
// path: an attacker-writable directory must never gain basename trust
// (an evil /tmp/ls is NOT ls).
var trustedProgramDirs = []string{
	"/bin", "/sbin", "/usr/bin", "/usr/sbin",
	"/usr/local/bin", "/usr/local/sbin", "/opt/homebrew/bin",
}

// isTrustedProgramDir reports whether dir is a candidate directory whose
// ownership and permissions actually justify basename trust: it must be
// root-owned and not writable by group or others. A static allowlist is
// not enough — /opt/homebrew/bin is owned by the daily login user (and
// group-writable) on any Homebrew machine, so a trojaned binary there
// would otherwise inherit the trust of bare-name bindings.
func isTrustedProgramDir(dir string) bool {
	listed := false
	for _, d := range trustedProgramDirs {
		if dir == d {
			listed = true
			break
		}
	}
	if !listed {
		return false
	}
	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		return false
	}
	return info.Mode().Perm()&0o022 == 0 && dirOwnedByRoot(info)
}

// normalizeTrustedArgv rewrites argv[0] to its basename when it lives in
// a verified trusted system directory, so bare-name bindings match
// absolute invocations. ok=false when no normalization applies.
func normalizeTrustedArgv(argv []string) ([]string, bool) {
	if len(argv) == 0 {
		return nil, false
	}
	dir := filepath.Dir(argv[0])
	base := filepath.Base(argv[0])
	if base == argv[0] {
		return nil, false // already bare
	}
	if isTrustedProgramDir(dir) {
		return append([]string{base}, argv[1:]...), true
	}
	return nil, false
}

// argvBindsPrefix reports whether argv matches a categorical prefix
// binding, honoring trusted-directory basename normalization on the
// first token.
func argvBindsPrefix(argv, prefix []string) bool {
	if ArgvHasPrefix(argv, prefix) {
		return true
	}
	if normalized, ok := normalizeTrustedArgv(argv); ok {
		return ArgvHasPrefix(normalized, prefix)
	}
	return false
}

// --- categorical memory prefix derivation ---

// subcommandToken matches simple subcommand words (test, run, vet) used
// to widen a binding from ["go"] to ["go", "test"] without allowing
// arbitrary scripting.
var subcommandToken = regexp.MustCompile(`^[a-z][a-z0-9_+-]*$`)

// neverPersistPrograms must never start a categorical memory: they
// compose or destroy arbitrarily and do not deserve a standing approval.
var neverPersistPrograms = map[string]struct{}{
	"rm": {}, "rmdir": {}, "unlink": {}, "sudo": {}, "su": {}, "doas": {},
	"dd": {}, "mkfs": {}, "shred": {}, "fdisk": {}, "diskutil": {},
	"newfs_hfs": {}, "hdiutil": {},
}

// interpreterPrograms are memory-eligible ONLY when invoking a script
// file (node scripts/lx.js) or a harmless informational flag, never for
// inline evaluation or REPL/stdin.
var interpreterPrograms = map[string]struct{}{
	"python": {}, "python3": {}, "node": {}, "ruby": {}, "perl": {},
}

// scriptFileToken reports whether arg looks like a script path rather
// than inline code or a flag.
func scriptFileToken(arg string) bool {
	if strings.ContainsAny(arg, `/\`) {
		return true
	}
	for _, ext := range []string{".js", ".mjs", ".cjs", ".ts", ".py", ".rb", ".pl"} {
		if strings.HasSuffix(arg, ext) {
			return true
		}
	}
	return false
}

// subcommandedPrograms are known subcommand-style tools for which the
// first positional argument is a categorical subcommand worth including
// (["go", "test"] instead of the too-broad ["go"]).
var subcommandedPrograms = map[string]struct{}{
	"go": {}, "npm": {}, "npx": {}, "pnpm": {}, "yarn": {}, "cargo": {},
	"git": {}, "bazel": {}, "docker": {}, "kubectl": {}, "helm": {}, "gh": {},
	"golangci-lint": {}, "ruff": {}, "eslint": {}, "tsc": {},
}

// DeriveMemoryPrefixes computes the categorical prefixes an exec call
// may be remembered by: one prefix for a plain argv, one PER STEP for a
// composed shell command. Derivable only when the plan is fully static
// and every step is individually derivable. ok=false means the call may
// only ever be remembered by its exact argv (indicated shapes) or not
// at all.
func DeriveMemoryPrefixes(d Derivation) ([][]string, bool) {
	if len(d.Plan.Steps) == 0 || !d.StaticPlan {
		return nil, false
	}
	var prefixes [][]string
	for i, argv := range d.Argvs {
		// A step that executes content not present in its argv (a
		// container image, a runtime-downloaded package, a cluster
		// payload, stdin-fed program text) must never be remembered
		// categorically: the prefix would bless every future payload.
		// StepEffects align with Argvs index-by-index on a static plan.
		if i < len(d.StepEffects) && d.StepEffects[i].OpaquePayload {
			return nil, false
		}
		prefix, ok := deriveArgvPrefix(argv)
		if !ok {
			return nil, false
		}
		duplicate := false
		for _, existing := range prefixes {
			if stringSliceEqual(existing, prefix) {
				duplicate = true
				break
			}
		}
		if !duplicate {
			prefixes = append(prefixes, prefix)
		}
	}
	if len(prefixes) == 0 {
		return nil, false
	}
	return prefixes, true
}

// deriveArgvPrefix computes the categorical prefix for one argv:
// [program] plus the first subcommand-like positional argument.
func deriveArgvPrefix(argv []string) ([]string, bool) {
	if len(argv) == 0 {
		return nil, false
	}
	program := argv[0]
	base := programBase(program)
	if _, banned := neverPersistPrograms[base]; banned {
		return nil, false
	}
	if _, shell := stdinExecPrograms[base]; shell {
		return nil, false // shells and interpreters-by-stdin
	}
	if _, interp := interpreterPrograms[base]; interp {
		if len(argv) < 2 {
			return nil, false
		}
		if strings.HasPrefix(argv[1], "-") {
			if _, harmless := informationalInterpreterFlags[argv[1]]; harmless {
				return []string{program, argv[1]}, true
			}
			return nil, false
		}
		if !scriptFileToken(argv[1]) {
			return nil, false
		}
		return []string{program, argv[1]}, true
	}
	prefix := []string{program}
	if _, subcommanded := subcommandedPrograms[base]; !subcommanded {
		return prefix, true
	}
	rest := argv[1:]
	if base == "git" {
		// git's global options (-C/-c/--git-dir/...) must be stripped
		// before the subcommand is visible; the shared parser answers.
		sub, gitRest, ok := splitGitGlobalOpts(rest)
		if !ok {
			return nil, false
		}
		if subcommandToken.MatchString(sub) {
			prefix = append(prefix, sub)
		}
		_ = gitRest
		return prefix, true
	}
	if len(rest) > 0 && strings.HasPrefix(rest[0], "-") {
		// Global options we cannot model: refuse the memory rather
		// than degrading to the bare program name — [git]-shaped
		// prefixes are far too wide to stand as user intent.
		return nil, false
	}
	for _, arg := range rest {
		if strings.HasPrefix(arg, "-") {
			break
		}
		if subcommandToken.MatchString(arg) {
			prefix = append(prefix, arg)
		}
		break
	}
	return prefix, true
}
