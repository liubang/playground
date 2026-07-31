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
// Created: 2026/07/27

package permission

import (
	"os"
	"strings"
)

// This is a heuristic screen, not a boundary: it reduces obviously
// dangerous commands slipping through the on-request auto-allow baseline.
// The real boundary remains the sandbox and the deny rules. Programs that
// compose arbitrarily (sh, bash, eval-form interpreters) are already R3
// via the tool's risk elevation and need no listing here.

// dangerousPrograms are destructive-at-any-target tools that always
// deserve a prompt regardless of arguments.
var dangerousPrograms = map[string]string{
	"dd":       "dd overwrites block devices/files byte-by-byte",
	"mkfs":     "mkfs formats a filesystem",
	"shred":    "shred destroys file contents irrecoverably",
	"fdisk":    "fdisk edits partition tables",
	"diskutil": "diskutil can erase/repartition disks",
	"sudo":     "sudo runs the command as root, escaping every user-level boundary",
	"su":       "su switches to another user (typically root), escaping every user-level boundary",
	"doas":     "doas runs the command as another user, escaping every user-level boundary",
}

// dangerousSubcommandOps are (program, subcommand) pairs whose flagged
// forms rewrite shared state or discard work: git push --force/--delete,
// git reset --hard.
var dangerousSubcommandOps = map[string]map[string][]string{
	"git": {
		"push":  {"--force", "-f", "--force-with-lease", "--delete", "-d"},
		"reset": {"--hard"},
	},
}

// exfilNetworkPrograms are network egress tools; combined with an argument
// pointing at a credential path they look like secret exfiltration. Inside
// the sandbox sensitive paths stay unreadable, but a widened sandbox
// (network grant) keeps that protection while the tool gains egress — so
// the pattern still deserves a prompt.
var exfilNetworkPrograms = map[string]struct{}{
	"curl": {}, "wget": {}, "nc": {}, "ncat": {}, "netcat": {}, "scp": {}, "rsync": {},
}

// credentialPathHints are path fragments whose presence in a network
// tool's argv suggests credential exfiltration.
var credentialPathHints = []string{
	".ssh", ".aws", ".kube", ".gnupg", ".mt_sso_config", "id_rsa", "id_ed25519",
	"credentials", "/etc/shadow", "/etc/passwd",
}

// DangerousCommand returns a human-readable reason when argv matches the
// dangerous-command heuristics, or "" when it does not. Matching commands
// are asked (never silently allowed) in every approval mode.
func DangerousCommand(argv []string) string {
	if len(argv) == 0 {
		return ""
	}
	base := argv[0]
	if idx := strings.LastIndexAny(base, `/\`); idx >= 0 {
		base = base[idx+1:]
	}
	base = strings.ToLower(base)
	if reason, ok := dangerousPrograms[base]; ok && reason != "" {
		return reason
	}
	if ops, ok := dangerousSubcommandOps[base]; ok {
		if reason := matchDangerousSubcommand(argv[1:], ops); reason != "" {
			return reason
		}
	}
	if reason := matchDangerousTargets(base, argv[1:]); reason != "" {
		return reason
	}
	if reason := matchGitClean(base, argv[1:]); reason != "" {
		return reason
	}
	if reason := matchCredentialExfil(base, argv[1:]); reason != "" {
		return reason
	}
	return ""
}

// matchGitClean flags `git clean` with a force flag (which deletes
// untracked files irrecoverably) but not the dry-run form (-n).
func matchGitClean(base string, args []string) string {
	if base != "git" {
		return ""
	}
	subcommand := false
	forced := false
	skipNext := false
	for _, arg := range args {
		if skipNext {
			skipNext = false
			continue
		}
		if strings.HasPrefix(arg, "-") {
			if _, takesValue := gitValueFlags[arg]; takesValue {
				skipNext = true
				continue
			}
			if subcommand && strings.ContainsRune(arg, 'f') && !strings.ContainsRune(arg, 'n') {
				forced = true
			}
			continue
		}
		if !subcommand {
			if arg != "clean" {
				return ""
			}
			subcommand = true
		}
	}
	if subcommand && forced {
		return "git clean -f deletes untracked files irrecoverably"
	}
	return ""
}

// matchCredentialExfil flags network egress tools whose arguments
// reference credential paths.
func matchCredentialExfil(base string, args []string) string {
	if _, ok := exfilNetworkPrograms[base]; !ok {
		return ""
	}
	for _, arg := range args {
		lower := strings.ToLower(arg)
		for _, hint := range credentialPathHints {
			if strings.Contains(lower, hint) {
				return base + " with a credential-path argument (" + arg + ") looks like secret exfiltration"
			}
		}
	}
	return ""
}

// gitValueFlags are global git options that consume the next token as
// their value; skipping them keeps `git -C /repo push --force` and
// `git -c x=y push -f` inside the danger screen.
var gitValueFlags = map[string]struct{}{
	"-C": {}, "-c": {}, "--git-dir": {}, "--work-tree": {}, "--namespace": {}, "--exec-path": {},
}

// matchDangerousSubcommand flags (subcommand, flag) pairs like
// git push --force.
func matchDangerousSubcommand(args []string, ops map[string][]string) string {
	skipNext := false
	for _, arg := range args {
		if skipNext {
			skipNext = false
			continue
		}
		if strings.HasPrefix(arg, "-") {
			if _, takesValue := gitValueFlags[arg]; takesValue {
				skipNext = true
			}
			continue
		}
		flags, ok := ops[arg]
		if !ok {
			return "" // first positional is not a dangerous subcommand
		}
		for _, rest := range args {
			for _, f := range flags {
				if rest == f {
					return "git " + arg + " " + f + " can rewrite or delete shared history"
				}
			}
		}
		return ""
	}
	return ""
}

// matchDangerousTargets flags recursive/catastrophic filesystem operations
// aimed at critical roots: rm -rf /, rm -rf ~, chmod -R 777 /, etc.
func matchDangerousTargets(base string, args []string) string {
	var recursive bool
	switch base {
	case "rm", "rmdir", "chmod", "chown", "chgrp":
	default:
		return ""
	}
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			if strings.ContainsAny(arg, "rR") || strings.HasPrefix(arg, "--recursive") {
				recursive = true
			}
			continue
		}
		if isCriticalRoot(arg) {
			return base + " targets a critical root (" + arg + ")"
		}
	}
	// rm -rf with no critical target is still worth a prompt when it is
	// clearly aiming above the working directory (../.. escape).
	if base == "rm" && recursive {
		for _, arg := range args {
			if strings.HasPrefix(arg, "../") || arg == ".." {
				return "rm -r escapes the working directory (" + arg + ")"
			}
		}
	}
	return ""
}

// isCriticalRoot reports whether arg denotes /, the user's home, or a
// top-level system directory.
func isCriticalRoot(arg string) bool {
	arg = strings.TrimSuffix(arg, "/")
	if arg == "" || arg == "/" || arg == "~" || arg == "/*" {
		return true
	}
	if home, err := os.UserHomeDir(); err == nil && home != "" && arg == home {
		return true
	}
	switch arg {
	case "/bin", "/sbin", "/usr", "/etc", "/var", "/System", "/Library", "/Users", "/boot", "/opt":
		return true
	}
	return false
}
