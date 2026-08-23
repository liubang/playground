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

// Semantic command understanding: for the command families that dominate
// real usage, the effect is derived from the command's SEMANTICS — its
// subcommand, normalized options, and target shape — not from matching
// text patterns. A deriver only answers for the forms it fully
// understands (whitelist-shaped parsing); anything else returns ok=false
// and the step degrades to unprovable, which is a bookkeeping state, not
// a verdict: unprovable + sandbox-confined still runs silently, while
// unprovable + boundary-crossing asks with the unprovability disclosed.
package permission

import (
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// semDeriveFunc derives one statically-resolved step's effect. ok=false
// means the invocation uses a form the table cannot fully explain.
type semDeriveFunc func(argv []string) (Effect, bool)

// semTable maps program basenames to their semantic deriver. Programs
// NOT listed here derive to the generic unprovable-confined effect:
// the sandbox bounds them, and any declared boundary crossing is judged
// on the declaration alone.
//
// The map is populated in init: several derivers recurse through
// deriveStepRec (find -exec, xargs), which would otherwise form an
// initialization cycle (table → deriver → deriveStepRec → table).
var semTable map[string]semDeriveFunc

func init() {
	semTable = map[string]semDeriveFunc{
		// git family (subcommand semantics, full option grammar)
		"git": semDeriveGit,
		// destructive-at-target filesystem programs
		"rm": semDeriveRm, "rmdir": semDeriveRm, "unlink": semDeriveRm,
		"chmod": semDeriveChmod, "chown": semDeriveChmod, "chgrp": semDeriveChmod,
		// destructive-at-any-target programs
		"dd": semDeriveAlwaysDestructive, "mkfs": semDeriveAlwaysDestructive,
		"shred": semDeriveAlwaysDestructive, "fdisk": semDeriveAlwaysDestructive,
		"diskutil": semDeriveAlwaysDestructive, "newfs_hfs": semDeriveAlwaysDestructive,
		"hdiutil": semDeriveAlwaysDestructive,
		// privilege escalation
		"sudo": semDerivePrivilegeEscalation, "su": semDerivePrivilegeEscalation,
		"doas": semDerivePrivilegeEscalation,
		// network tools
		"curl": semDeriveCurl, "wget": semDeriveWget,
		"nc": semDeriveNetcat, "ncat": semDeriveNetcat, "netcat": semDeriveNetcat,
		"scp": semDeriveScp, "rsync": semDeriveRsync, "ssh": semDeriveSSH,
		// interpreters (eval forms, script files, stdin programs)
		"python": semDeriveInterpreter, "python3": semDeriveInterpreter,
		"node": semDeriveInterpreter, "ruby": semDeriveInterpreter, "perl": semDeriveInterpreter,
		// indirect executors
		"find": semDeriveFind, "xargs": semDeriveXargs,
		// macOS automation / persistence surfaces
		"osascript": semDeriveOsascript, "launchctl": semDeriveLaunchctl,
		"crontab": semDeriveCrontab,
		// package managers / build entry points with shared-state forms
		"npm": semDeriveNpm, "pnpm": semDeriveNpm, "yarn": semDeriveNpm,
		"go": semDeriveGo, "docker": semDeriveDocker, "kubectl": semDeriveKubectl,
	}
}

// maxDeriveDepth bounds recursive derivation (find -exec find ...,
// wrapper chains) so a pathological command cannot recurse forever.
const maxDeriveDepth = 8

// deriveStep computes one plan step's effect.
func deriveStep(step ExecStep) Effect {
	return deriveStepRec(step, 0)
}

func deriveStepRec(step ExecStep, depth int) Effect {
	if depth > maxDeriveDepth {
		return Effect{Proven: false, Reason: "command nesting too deep to analyze"}
	}
	if len(step.Argv) == 0 {
		return Effect{Proven: false, Reason: "command words contain dynamic expansions"}
	}
	argv := step.Argv
	base := programBase(argv[0])

	// A program that executes its stdin gets that checked first: the
	// stdin content is the program, and it is not in the argv.
	if step.Stdin != process.StdinNone {
		if e, handled := deriveStdinProgram(base, step); handled {
			return e
		}
	}

	// A nested shell -c invocation (inside a pipe, a substitution, or
	// after a wrapper): analyze the inner script recursively instead
	// of treating the shell as an opaque program.
	if process.IsShellProgram(argv[0]) {
		if script, ok := process.ShellScriptForm(argv); ok {
			sub := normalizeShellScript(script, argv, depth)
			return derivePlan(sub, depth+1)
		}
		return Effect{
			Proven: false,
			Reason: base + " invocation is not an analyzable -c form (script file or unrecognized options)",
		}
	}

	// Wrapper programs change how a command runs, not what it does:
	// strip and derive the wrapped command. The stdin shape rides
	// along — `curl x | env sh` still feeds sh's stdin.
	if rest, ok := stripWrapper(base, argv[1:]); ok {
		return deriveStepRec(ExecStep{
			Argv:          rest,
			Stdin:         step.Stdin,
			Heredoc:       step.Heredoc,
			HeredocStatic: step.HeredocStatic,
		}, depth+1)
	}

	if derive, ok := semTable[base]; ok {
		e, explained := derive(argv)
		if !explained {
			return Effect{
				Proven: false,
				Reason: base + " invocation uses an option form the semantic table cannot fully explain",
			}
		}
		return e
	}
	return Effect{
		Proven: false,
		Reason: "unrecognized program " + base + " — effects not provable from argv",
	}
}

// deriveStdinProgram handles commands whose stdin IS the program: a pipe,
// heredoc, or here-string feeding an interpreter. Shell consumers with a
// static heredoc never reach here — NormalizeExec already inlined the
// body. ok=false means stdin is data for this command (a script file was
// given), and derivation continues normally.
func deriveStdinProgram(base string, step ExecStep) (Effect, bool) {
	if _, interpreter := stdinExecPrograms[base]; !interpreter {
		return Effect{}, false
	}
	if consumerRunsScriptFile(base, step.Argv) {
		return Effect{}, false
	}
	reason := base + " executes a program arriving on stdin — content not statically analyzable"
	switch step.Stdin {
	case process.StdinHeredoc:
		reason = base + " executes a heredoc-fed program whose content is not statically analyzable"
	case process.StdinWord:
		reason = base + " executes a here-string program whose content is not statically analyzable"
	}
	return Effect{
		Proven:     false,
		Reason:     reason,
		Indicators: []string{base + " executes program text from its stdin (pipe/heredoc) — the code cannot be screened"},
	}, true
}

// stdinExecPrograms are shells and interpreters that execute their stdin
// when invoked without a script file.
var stdinExecPrograms = map[string]struct{}{
	"sh": {}, "bash": {}, "zsh": {}, "dash": {}, "ksh": {}, "fish": {},
	"python": {}, "python3": {}, "node": {}, "perl": {}, "ruby": {},
}

// consumerRunsScriptFile reports whether the command's PROGRAM comes
// from its argv — an inline eval form (-c/-e/--eval) or a script-file
// positional — in which case stdin is data, not code. A bare "-" is an
// explicit stdin marker, and a shell's -s flag forces stdin execution
// even when positionals follow (sh -s -- args runs the PIPED script).
func consumerRunsScriptFile(program string, argv []string) bool {
	if len(argv) < 2 {
		return false
	}
	isShell := process.IsShellProgram(program)
	for _, arg := range argv[1:] {
		if arg == "-" {
			return false // explicit stdin marker
		}
		if strings.HasPrefix(arg, "-") {
			if arg == "--eval" {
				return true
			}
			if !strings.HasPrefix(arg, "--") {
				letters := arg[1:]
				if strings.ContainsRune(letters, 'c') || strings.ContainsRune(letters, 'e') {
					return true // inline eval: the program is in argv
				}
				if isShell && strings.ContainsRune(letters, 's') {
					return false // -s forces stdin execution
				}
			}
			continue
		}
		if isShell {
			return true // first positional is the script file
		}
		return scriptFileToken(arg)
	}
	return false
}

// stripWrapper peels one wrapper program (env, nice, nohup, timeout,
// stdbuf, time, command, builtin) off argv, returning the wrapped
// command. ok=false when argv is not a wrapper invocation. Unknown flag
// forms fail closed (ok=false → the wrapper itself is unrecognized →
// unprovable), never guessed.
func stripWrapper(base string, args []string) ([]string, bool) {
	switch base {
	case "env":
		// env [-i] [-u NAME]... [NAME=value]... cmd args...
		for i := 0; i < len(args); i++ {
			arg := args[i]
			switch {
			case arg == "-i" || arg == "--ignore-environment" || arg == "-0" || arg == "--null":
				continue
			case arg == "-u" || arg == "--unset":
				i++ // consumes the next token
			case strings.HasPrefix(arg, "--unset="):
				continue
			case strings.HasPrefix(arg, "-"):
				return nil, false // unknown env flag: do not guess
			case strings.Contains(arg, "="):
				continue // NAME=value assignment
			default:
				return args[i:], true
			}
		}
		return nil, false
	case "nice", "nohup", "stdbuf", "timeout", "time", "command", "builtin":
		skipPositional := base == "timeout"
		for i := 0; i < len(args); i++ {
			arg := args[i]
			if !strings.HasPrefix(arg, "-") {
				if skipPositional {
					skipPositional = false
					continue
				}
				return args[i:], true
			}
			switch base {
			case "nice":
				if arg == "-n" || arg == "--adjustment" {
					i++
				}
			case "stdbuf":
				if arg == "-i" || arg == "-o" || arg == "-e" {
					i++
				}
			case "timeout":
				if arg == "-k" || arg == "--kill-after" || arg == "-s" || arg == "--signal" {
					i++
				}
			}
		}
		return nil, false
	}
	return nil, false
}

// programBase renders a program token as its lowercase basename.
func programBase(program string) string {
	if idx := strings.LastIndexAny(program, `/\`); idx >= 0 {
		program = program[idx+1:]
	}
	return strings.ToLower(program)
}
