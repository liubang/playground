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
// Created: 2026/08/09

// Script-level danger screening: patterns that only exist ACROSS commands
// (a pipeline feeding an interpreter, a redirect into a sensitive path)
// or nested inside shell constructs (a substitution hiding `rm -rf`).
// The analysis (process.ShellAnalysis) has already flattened the script
// into per-command argvs, so each subcommand flows through the same
// argv-level screen as a plain invocation — nothing hides inside
// `$(...)` or `( ... )` anymore.
package permission

import (
	"os"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// stdinExecPrograms are shells and interpreters that execute their stdin
// when invoked without a script file. A pipeline ending in one of these
// is the classic remote-code-execution shape (curl ... | sh).
var stdinExecPrograms = map[string]struct{}{
	"sh": {}, "bash": {}, "zsh": {}, "dash": {}, "ksh": {}, "fish": {},
	"python": {}, "python3": {}, "node": {}, "perl": {}, "ruby": {},
}

// DangerousScript returns a human-readable reason when a composed shell
// script matches the script-level danger heuristics, or "" when it does
// not. It complements (never replaces) the per-argv screen: every
// subcommand is ALSO run through DangerousCommand here, so the danger
// decider has a single entry point for shell invocations.
func DangerousScript(a *process.ShellAnalysis) string {
	if a == nil {
		return ""
	}
	for _, cmd := range a.Commands {
		if len(cmd.Argv) == 0 {
			continue // dynamic words: no literal argv to screen
		}
		if reason := DangerousCommand(cmd.Argv); reason != "" {
			return reason
		}
	}
	for _, pipe := range a.Pipes {
		if reason := dangerousPipe(pipe); reason != "" {
			return reason
		}
	}
	for _, target := range a.WriteRedirects {
		if reason := dangerousRedirectTarget(target); reason != "" {
			return reason
		}
	}
	return ""
}

// dangerousPipe flags pipelines whose consumer executes its stdin:
// `curl ... | sh`, `base64 -d | python3`. A consumer with a script-file
// argument (python3 analyze.py) runs a fixed program, not the pipe.
func dangerousPipe(pipe process.PipeEdge) string {
	if pipe.Consumer == "" {
		return ""
	}
	if _, exec := stdinExecPrograms[pipe.Consumer]; !exec {
		return ""
	}
	if consumerRunsScriptFile(pipe.Consumer, pipe.ConsumerArgv) {
		return ""
	}
	return "pipe into " + pipe.Consumer + " executes the piped stream as code (remote code execution pattern)"
}

// consumerRunsScriptFile reports whether the consumer argv carries a
// positional script-file argument, meaning stdin is data, not code.
// Eval forms (-c, -e, --eval) execute inline code and do NOT count.
func consumerRunsScriptFile(program string, argv []string) bool {
	if len(argv) == 0 {
		return false
	}
	if _, isShell := map[string]struct{}{
		"sh": {}, "bash": {}, "zsh": {}, "dash": {}, "ksh": {}, "fish": {},
	}[program]; isShell {
		// sh script.sh runs the file; sh -c / bare sh executes stdin/code.
		for _, arg := range argv[1:] {
			if strings.HasPrefix(arg, "-") {
				continue
			}
			return true // first positional is the script file
		}
		return false
	}
	// Interpreters: a positional that looks like a script path means the
	// pipe is input data; anything else (flags only, -c/-e forms) does not.
	for _, arg := range argv[1:] {
		if strings.HasPrefix(arg, "-") {
			continue
		}
		return scriptFileToken(arg)
	}
	return false
}

// dangerousRedirectTarget flags file-writing redirects aimed at shell
// startup files, credential locations, git metadata, or system paths —
// the persistence and tampering vectors a redirect can reach that a plain
// argv scan never sees.
func dangerousRedirectTarget(target string) string {
	if target == "" {
		return ""
	}
	expanded := target
	if strings.HasPrefix(expanded, "~/") || expanded == "~" {
		if home, err := os.UserHomeDir(); err == nil {
			expanded = filepath.Join(home, strings.TrimPrefix(expanded, "~"))
		}
	}
	clean := filepath.Clean(expanded)
	lower := strings.ToLower(clean)
	base := filepath.Base(lower)

	// Shell startup and account-level config: writing here executes code
	// on the user's next login/shell, outside any sandbox.
	switch base {
	case ".zshrc", ".bashrc", ".bash_profile", ".bash_login", ".profile",
		".zprofile", ".zshenv", ".zlogin", ".gitconfig", ".git-credentials",
		".netrc", ".npmrc", ".pypirc":
		return "redirect writes to " + target + " (user-level config tampering / persistence)"
	}
	// Credential directories anywhere in the path.
	for _, hint := range credentialPathHints {
		if strings.Contains(lower, strings.ToLower(hint)) {
			return "redirect writes to a credential path (" + target + ")"
		}
	}
	// Repository metadata: hooks and config are the git-level persistence
	// vectors (a planted hook runs when the USER later invokes git
	// outside the sandbox). Seatbelt denies these writes as well; the
	// screen exists so the attempt is surfaced, not just blocked. The
	// check is segment-based so submodule/worktree gitdirs
	// (.git/modules/<name>/hooks, .git/worktrees/<name>/config) match too.
	if gitMetaWrite(lower) {
		return "redirect writes into protected git metadata (" + target + "): hooks/config can escalate beyond the sandbox"
	}
	sep := string(filepath.Separator)
	if strings.Contains(lower, sep+".loom") || strings.HasPrefix(lower, ".loom"+sep) || lower == ".loom" {
		return "redirect writes into protected loom metadata (" + target + ")"
	}
	if isCriticalRoot(clean) {
		return "redirect targets a critical root (" + target + ")"
	}
	return ""
}

// gitMetaWrite reports whether a (lowercased, cleaned) path writes into a
// .git directory's hooks or config: any "hooks" segment after the .git
// segment, or a trailing "config" directly under .git (one level of
// nesting tolerated for worktrees/modules is covered by the hooks rule;
// config sits exactly one level below .git or two for
// modules/worktrees).
func gitMetaWrite(path string) bool {
	segs := strings.Split(filepath.ToSlash(path), "/")
	for i, seg := range segs {
		if seg != ".git" {
			continue
		}
		rest := segs[i+1:]
		for _, r := range rest {
			if r == "hooks" {
				return true
			}
		}
		if len(rest) > 0 && rest[len(rest)-1] == "config" {
			return true
		}
	}
	return false
}
