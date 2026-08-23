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

// Semantic derivation for git: global options are consumed by git's own
// grammar (so `git -C /repo push --force` classifies exactly like
// `git push --force`), then the subcommand + its normalized flags map to
// a consequence class. Combined short flags (-fu), --flag=value, and
// unambiguous long abbreviations all normalize away — textual variation
// cannot lower the consequence.
package permission

import "strings"

// gitGlobalOpts is the option grammar of git itself (before the
// subcommand). Value-taking options consume the next token.
var gitGlobalOpts = OptTable{
	Long: map[string]bool{
		"git-dir": true, "work-tree": true, "namespace": true,
		"exec-path": true, "config-env": true, "html-path": false,
		"man-path": false, "info-path": false, "paginate": false,
		"no-pager": false, "no-replace-objects": false, "bare": false,
		"literal-pathspecs": false, "glob-pathspecs": false,
		"noglob-pathspecs": false, "icase-pathspecs": false,
		"no-optional-locks": false, "no-advice": false,
		"version": false, "help": false, "list-cmds": true,
	},
	Short: map[rune]bool{'C': true, 'c': true, 'p': false, 'P': false},
}

// gitSubcommandEffect is the static effect of one subcommand: network
// need plus consequence, before flag-specific elevation.
type gitSubcommandEffect struct {
	network     bool
	consequence Consequence
}

// gitSubcommands enumerates every subcommand this table understands.
// Anything else derives to unprovable (never to "safe").
var gitSubcommands = map[string]gitSubcommandEffect{
	// read-only, local
	"status": {}, "log": {}, "diff": {}, "show": {}, "blame": {},
	"grep": {}, "rev-parse": {}, "ls-files": {}, "ls-tree": {},
	"describe": {}, "shortlog": {}, "reflog": {}, "name-rev": {},
	"cat-file": {}, "count-objects": {}, "var": {},
	"stash": {},
	// local state changes, recoverable through git itself
	"add": {}, "commit": {}, "merge": {}, "rebase": {}, "cherry-pick": {},
	"revert": {}, "tag": {}, "mv": {}, "rm": {}, "apply": {},
	"am": {}, "format-patch": {}, "worktree": {}, "submodule": {},
	"switch": {}, "notes": {}, "bisect": {}, "update-index": {},
	"checkout-index": {}, "read-tree": {}, "write-tree": {},
	"commit-tree": {}, "update-ref": {}, "symbolic-ref": {},
	// network, non-mutating remotely
	"fetch": {network: true}, "pull": {network: true},
	"clone": {network: true}, "ls-remote": {network: true},
	// shared state
	"push": {network: true, consequence: ConsequenceSharedState},
}

// splitGitGlobalOpts consumes git's global options (per gitGlobalOpts)
// and returns the subcommand and its arguments. ok=false when the
// options cannot be fully explained or no subcommand follows. It is the
// single parser for git's global grammar — the semantic deriver and
// the memory prefix deriver share it, so the two can never drift.
func splitGitGlobalOpts(args []string) (subcommand string, rest []string, ok bool) {
	i := 0
	for ; i < len(args); i++ {
		a := args[i]
		if a == "--" {
			i++
			break
		}
		if !strings.HasPrefix(a, "-") {
			break
		}
		if strings.HasPrefix(a, "--") {
			name := strings.TrimPrefix(strings.SplitN(a, "=", 2)[0], "--")
			takesValue, known := gitGlobalOpts.Long[name]
			if !known {
				return "", nil, false
			}
			if takesValue && !strings.Contains(a, "=") {
				i++
			}
			continue
		}
		for _, r := range a[1:] {
			takesValue, known := gitGlobalOpts.Short[r]
			if !known {
				return "", nil, false
			}
			if takesValue {
				i++
				break
			}
		}
	}
	if i >= len(args) {
		return "", nil, false // git with no subcommand
	}
	return args[i], args[i+1:], true
}

// sensitiveGitConfigKeys are config keys whose injection redirects
// code execution: hooks paths, ssh commands, protocol escape hatches.
var sensitiveGitConfigKeys = []string{"core.hookspath", "core.sshcommand", "core.gitproxy"}

// gitConfigInjection reports the indicator when a -c/--config-env
// option injects a sensitive config key.
func gitConfigInjection(argv []string) string {
	for i, a := range argv {
		var kv string
		switch {
		case a == "-c" && i+1 < len(argv):
			kv = argv[i+1]
		case strings.HasPrefix(a, "--config-env="):
			kv = strings.TrimPrefix(a, "--config-env=")
		default:
			continue
		}
		key, _, _ := strings.Cut(kv, "=")
		key = strings.ToLower(key)
		for _, sensitive := range sensitiveGitConfigKeys {
			if key == sensitive {
				return "git config injection via " + a + " (" + key + " can redirect code execution)"
			}
		}
		if strings.HasPrefix(key, "protocol.") && strings.HasSuffix(key, ".allow") {
			return "git config injection via " + a + " (protocol allow-listing enables arbitrary scheme execution)"
		}
	}
	return ""
}

// semDeriveGit derives the effect of a git invocation.
func semDeriveGit(argv []string) (Effect, bool) {
	sub, rest, ok := splitGitGlobalOpts(argv[1:])
	if !ok {
		return Effect{}, false
	}
	if indicator := gitConfigInjection(argv[1:]); indicator != "" {
		return Effect{
			Proven:      true,
			Consequence: ConsequenceLocalDestructive,
			Reason:      "git with a sensitive config injection",
			Indicators:  []string{indicator},
		}, true
	}

	switch sub {
	case "push":
		return semDeriveGitPush(rest)
	case "reset":
		return semDeriveGitReset(rest)
	case "clean":
		return semDeriveGitClean(rest)
	case "branch":
		return semDeriveGitBranch(rest)
	case "checkout":
		return semDeriveGitCheckout(rest)
	case "restore":
		return semDeriveGitRestore(rest)
	case "config":
		return semDeriveGitConfig(rest)
	case "stash":
		return semDeriveGitStash(rest)
	}

	known, ok := gitSubcommands[sub]
	if !ok {
		return Effect{
			Proven: false,
			Reason: "unrecognized git subcommand " + sub,
		}, true
	}
	e := Effect{Proven: true, Consequence: known.consequence, Reason: "git " + sub}
	if known.network {
		e.Network = HostSet{Any: true}
	}
	return e, true
}

// gitPushOpts is push's option grammar.
var gitPushOpts = OptTable{
	Long: map[string]bool{
		"all": false, "branches": false, "mirror": false, "tags": false,
		"follow-tags": false, "atomic": false, "dry-run": false,
		"porcelain": false, "delete": false, "force": false,
		"force-with-lease": false, "force-if-includes": false,
		"no-force-if-includes": false, "verify": false, "no-verify": false,
		"progress": false, "no-progress": false, "quiet": false,
		"verbose": false, "set-upstream": false, "repo": true,
		"push-option": true, "receive-pack": true, "exec": true,
		"signed": false, "no-signed": false, "thin": false, "no-thin": false,
		"recurse-submodules": true, "ipv4": false, "ipv6": false,
	},
	Short: map[rune]bool{
		'd': false, 'f': false, 'n': false, 'q': false, 'v': false,
		'u': false, 'o': true, '4': false, '6': false,
	},
}

// semDeriveGitPush classifies push: always shared-state with network;
// force/delete forms (flags or refspec syntax) are shared-destructive.
func semDeriveGitPush(args []string) (Effect, bool) {
	opts, ok := ParseOpts(args, gitPushOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{
		Proven:      true,
		Network:     HostSet{Any: true},
		Consequence: ConsequenceSharedState,
		Reason:      "git push",
	}
	// --mirror implies force-updating every remote ref: destructive
	// by construction.
	destructive := opts.Has("--force", "-f", "--force-with-lease", "--delete", "-d", "--mirror")
	if !destructive {
		// Refspec syntax: ":dst" deletes a remote ref, "+src:dst"
		// (or a leading + in any position) forces the update.
		// positional[0], when present, is the remote.
		for _, pos := range opts.Positional {
			if strings.HasPrefix(pos, ":") || strings.HasPrefix(pos, "+") {
				destructive = true
				break
			}
		}
	}
	if destructive {
		e.Consequence = ConsequenceSharedDestructive
		e.Reason = "git push with force/delete — rewrites or removes remote history"
	}
	return e, true
}

// gitResetOpts is reset's option grammar.
var gitResetOpts = OptTable{
	Long: map[string]bool{
		"soft": false, "mixed": false, "hard": false, "merge": false,
		"keep": false, "patch": false, "quiet": false, "verbose": false,
		"intent-to-add": false, "pathspec-from-file": true,
		"pathspec-file-nul": false,
	},
	Short: map[rune]bool{'p': false, 'q': false, 'v': false, 'N': false},
}

func semDeriveGitReset(args []string) (Effect, bool) {
	opts, ok := ParseOpts(args, gitResetOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git reset"}
	if opts.Has("--hard") {
		e.Consequence = ConsequenceLocalDestructive
		e.Reason = "git reset --hard discards uncommitted work irreversibly"
	}
	return e, true
}

// gitCleanOpts is clean's option grammar.
var gitCleanOpts = OptTable{
	Long: map[string]bool{
		"force": false, "dry-run": false, "quiet": false, "interactive": false,
		"exclude": true, "exclude-from": true,
	},
	Short: map[rune]bool{'f': false, 'd': false, 'n': false, 'q': false, 'x': false, 'X': false, 'i': false, 'e': true},
}

func semDeriveGitClean(args []string) (Effect, bool) {
	opts, ok := ParseOpts(args, gitCleanOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git clean"}
	if opts.Has("--force", "-f") && !opts.Has("--dry-run", "-n") {
		e.Consequence = ConsequenceLocalDestructive
		e.Reason = "git clean -f deletes untracked files irreversibly"
	}
	return e, true
}

// gitBranchOpts is branch's option grammar.
var gitBranchOpts = OptTable{
	Long: map[string]bool{
		"delete": false, "force": false, "move": false, "copy": false,
		"remotes": false, "all": false, "list": false, "verbose": false,
		"quiet": false, "set-upstream-to": true, "unset-upstream": false,
		"track": false, "no-track": false, "contains": true,
		"no-contains": true, "merged": true, "no-merged": true,
		"sort": true, "format": true, "abbrev": true, "column": true,
		"no-column": false, "color": true, "no-color": false,
		"show-current": false, "edit-description": false,
		"points-at": true, "create-reflog": false, "ignore-case": false,
		"omit-empty": false, "recurse-submodules": false,
	},
	Short: map[rune]bool{
		'd': false, 'D': false, 'f': false, 'm': false, 'M': false,
		'c': false, 'C': false, 'r': false, 'a': false, 'v': false,
		'q': false, 'l': false, 'u': true, 't': false,
	},
}

func semDeriveGitBranch(args []string) (Effect, bool) {
	opts, ok := ParseOpts(args, gitBranchOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git branch"}
	// -D force-deletes a branch regardless of merge state, and -f
	// rewrites an existing branch pointer: both can orphan work
	// unrecoverably. A plain -d is a checked delete and stays confined.
	if opts.Has("-D") || opts.Has("-f", "--force") {
		e.Consequence = ConsequenceLocalDestructive
		e.Reason = "git branch -D/-f discards branch history that may be unrecoverable"
	}
	return e, true
}

// gitCheckoutOpts is checkout's option grammar.
var gitCheckoutOpts = OptTable{
	Long: map[string]bool{
		"detach": false, "force": false, "track": false, "no-track": false,
		"orphan": true, "branch": true, "new-branch": true, "ours": false,
		"theirs": false, "merge": false, "conflict": true, "patch": false,
		"quiet": false, "verbose": false, "progress": false,
		"no-progress": false, "ignore-other-worktrees": false,
		"overwrite-ignore": false, "recurse-submodules": false,
		"no-recurse-submodules": false, "guess": false, "no-guess": false,
		"pathspec-from-file": true, "pathspec-file-nul": false,
	},
	Short: map[rune]bool{'b': true, 'B': true, 'd': false, 'f': false, 't': false, 'p': false, 'q': false, 'v': false, 'm': false},
}

func semDeriveGitCheckout(args []string) (Effect, bool) {
	opts, ok := ParseOpts(args, gitCheckoutOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git checkout"}
	// `checkout -- <paths>` discards working-tree changes to those
	// paths; -f discards ALL local changes on a branch switch; -B
	// resets an existing branch pointer.
	if opts.Has("-f", "--force", "-B") {
		e.Consequence = ConsequenceLocalDestructive
		e.Reason = "git checkout -f/-B discards uncommitted changes or rewrites a branch"
		return e, true
	}
	for i, a := range args {
		if a == "--" && i+1 < len(args) {
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = "git checkout -- <paths> discards uncommitted changes to those paths"
			return e, true
		}
	}
	return e, true
}

// semDeriveGitStash classifies stash: drop/clear destroy stashed work
// irreversibly; every other form is confined.
func semDeriveGitStash(args []string) (Effect, bool) {
	for _, a := range args {
		if a == "drop" || a == "clear" {
			return Effect{
				Proven:      true,
				Consequence: ConsequenceLocalDestructive,
				Reason:      "git stash " + a + " destroys stashed work irreversibly",
			}, true
		}
		if !strings.HasPrefix(a, "-") {
			break
		}
	}
	return Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git stash"}, true
}

// gitRestoreOpts is restore's option grammar.
var gitRestoreOpts = OptTable{
	Long: map[string]bool{
		"worktree": false, "staged": false, "source": true, "patch": false,
		"ours": false, "theirs": false, "merge": false, "conflict": true,
		"ignore-unmerged": false, "progress": false, "no-progress": false,
		"quiet": false, "verbose": false, "pathspec-from-file": true,
		"pathspec-file-nul": false, "ignore-skip-worktree-bits": false,
	},
	Short: map[rune]bool{'W': false, 'S': false, 's': true, 'p': false, 'q': false, 'v': false, 'm': false},
}

func semDeriveGitRestore(args []string) (Effect, bool) {
	opts, ok := ParseOpts(args, gitRestoreOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git restore"}
	// Restoring the worktree (the default) discards uncommitted
	// changes; --staged alone only resets the index.
	if opts.Has("--worktree", "-W") || !opts.Has("--staged", "-S") {
		e.Consequence = ConsequenceLocalDestructive
		e.Reason = "git restore discards uncommitted changes in the worktree"
	}
	return e, true
}

// semDeriveGitConfig classifies config: writing repository config is
// workspace-confined (and .git/config is sandbox-protected), but
// core.hooksPath is a code-execution persistence vector — surfaced as an
// indicator so it can never be silently remembered.
func semDeriveGitConfig(args []string) (Effect, bool) {
	for _, a := range args {
		if strings.EqualFold(a, "core.hooksPath") || strings.HasPrefix(strings.ToLower(a), "core.hookspath=") {
			return Effect{
				Proven:      true,
				Consequence: ConsequenceLocalDestructive,
				Reason:      "git config core.hooksPath redirects hook execution to an arbitrary path",
				Indicators:  []string{"redirects git hook execution to an arbitrary path (persistence / code execution)"},
			}, true
		}
	}
	return Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "git config"}, true
}
