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

// Heuristic danger indicators — the demoted blacklist. Indicators no
// longer produce verdicts; they annotate an Effect with shapes whose
// real-world base rate of malice is high and whose risk the capability
// fields cannot fully express (remote code fed to an interpreter,
// persistence vectors). An indicated effect is never silently covered by
// a categorical package: only an exact-binding approval of the same
// shape covers it.
package permission

import (
	"os"
	"path/filepath"
	"strings"
)

// networkProducers are programs whose pipe output may be remote content.
var networkProducers = map[string]struct{}{
	"curl": {}, "wget": {}, "nc": {}, "ncat": {}, "netcat": {},
	"ssh": {}, "scp": {}, "rsync": {},
}

// planIndicators computes the cross-command indicators of a normalized
// plan: network content piped into an interpreter (the classic
// remote-code-execution shape).
func planIndicators(plan ExecPlan) []string {
	var out []string
	for _, pipe := range plan.Pipes {
		if pipe.Consumer == "" {
			continue
		}
		if _, interpreter := stdinExecPrograms[pipe.Consumer]; !interpreter {
			continue
		}
		if consumerRunsScriptFile(pipe.Consumer, pipe.ConsumerArgv) {
			continue
		}
		if _, remote := networkProducers[pipe.Producer]; remote {
			out = append(out,
				"pipe feeds network content from "+pipe.Producer+" into "+pipe.Consumer+
					" (remote code execution pattern)")
		}
	}
	return out
}

// sensitiveRedirectTarget returns an indicator when a write-redirect
// target is a persistence or tampering vector: shell startup files,
// account-level configs, credential locations, git metadata (hooks run
// when the USER later invokes git outside the sandbox), loom state, or
// a critical root. The sandbox denies these writes as well; the
// indicator exists so the ATTEMPT is surfaced, not just blocked, and so
// the shape can never be categorically remembered.
func sensitiveRedirectTarget(cleanAbsTarget string) string {
	lower := strings.ToLower(filepath.Clean(cleanAbsTarget))
	base := filepath.Base(lower)

	// Shell startup and account-level config: writing here executes
	// code on the user's next login/shell, outside any sandbox.
	switch base {
	case ".zshrc", ".bashrc", ".bash_profile", ".bash_login", ".profile",
		".zprofile", ".zshenv", ".zlogin", ".gitconfig", ".git-credentials",
		".netrc", ".npmrc", ".pypirc":
		return "writes to " + cleanAbsTarget + " (user-level config tampering / persistence)"
	}
	// Credential directories anywhere in the path.
	for _, hint := range credentialPathHints {
		if strings.Contains(lower, strings.ToLower(hint)) {
			return "writes to a credential path (" + cleanAbsTarget + ")"
		}
	}
	// Repository metadata: hooks and config are the git-level
	// persistence vectors. The check is segment-based so
	// submodule/worktree gitdirs match too.
	if gitMetaWrite(lower) {
		return "writes into protected git metadata (" + cleanAbsTarget + "): hooks/config can escalate beyond the sandbox"
	}
	sep := string(filepath.Separator)
	if strings.Contains(lower, sep+".loom") || strings.HasPrefix(lower, ".loom"+sep) || lower == ".loom" {
		return "writes into protected loom metadata (" + cleanAbsTarget + ")"
	}
	if isCriticalRoot(cleanAbsTarget) {
		return "writes to a critical root (" + cleanAbsTarget + ")"
	}
	return ""
}

// gitMetaWrite reports whether a (lowercased, cleaned) path writes into a
// .git directory's hooks or config: any "hooks" segment after the .git
// segment, or a trailing "config" directly under .git.
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

// homeDir resolves the user's home directory (indirection for tests).
var homeDir = os.UserHomeDir
