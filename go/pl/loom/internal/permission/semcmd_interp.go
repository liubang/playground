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

// Semantic derivation for interpreters, indirect executors (find/xargs),
// and macOS automation surfaces. Interpreters are honest about their
// limits: inline code and script-file content are not statically
// analyzable, so those forms derive to unprovable — sandbox-confined
// unprovable calls still run silently (the sandbox is the boundary), but
// any declared boundary crossing asks with the unprovability disclosed.
package permission

import (
	"strings"
)

// informationalInterpreterFlags print and exit without executing user
// code; they are the only fully-provable interpreter invocations.
var informationalInterpreterFlags = map[string]struct{}{
	"-v": {}, "--version": {}, "-V": {}, "-h": {}, "--help": {},
}

// semDeriveInterpreter classifies python/node/ruby/perl invocations by
// where the program comes from: a harmless informational flag (proven,
// confined), inline code or a script file (unprovable — the executed
// content is not in the argv's statically provable shape... it IS in
// argv for -c, but it is a different LANGUAGE the analyzer does not
// parse), or stdin/REPL (unprovable).
func semDeriveInterpreter(argv []string) (Effect, bool) {
	base := programBase(argv[0])
	if len(argv) < 2 {
		return Effect{
			Proven: false,
			Reason: base + " with no arguments reads a program from stdin/REPL",
		}, true
	}
	first := argv[1]
	if strings.HasPrefix(first, "-") {
		if _, harmless := informationalInterpreterFlags[first]; harmless {
			return Effect{
				Proven: true, Consequence: ConsequenceConfined,
				Reason: base + " " + first + " prints information and exits",
			}, true
		}
		return Effect{
			Proven: false,
			Reason: base + " executes inline code (" + first + ") that is not statically analyzable",
		}, true
	}
	if !scriptFileToken(first) {
		return Effect{
			Proven: false,
			Reason: base + " invocation form is not statically analyzable",
		}, true
	}
	return Effect{
		Proven: false,
		Reason: base + " executes the script file " + first + " (content not analyzed)",
	}, true
}

// semDeriveFind classifies find: read-only by default; -delete is
// local-destructive; -exec/-execdir/-ok payloads are derived RECURSIVELY
// (the payload argv is statically present, so its effect is provable),
// and a dynamic payload degrades the whole invocation to unprovable.
func semDeriveFind(argv []string) (Effect, bool) {
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "find"}
	args := argv[1:]
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch a {
		case "-delete":
			e.Consequence = ConsequenceLocalDestructive
			e.Reason = "find -delete removes matched files irreversibly"
		case "-exec", "-execdir", "-ok", "-okdir":
			payload := []string{}
			for j := i + 1; j < len(args); j++ {
				if args[j] == ";" || args[j] == "+" {
					break
				}
				payload = append(payload, args[j])
			}
			if len(payload) == 0 {
				return Effect{}, false
			}
			// The payload argv is static, so its effect is provable —
			// but the MATCHED FILES arrive at runtime, so targets are
			// dynamic even when the payload program is known. A
			// deletion-capable payload (rm/shred/dd) is destructive at
			// dynamic targets by construction.
			sub := deriveStepRec(ExecStep{Argv: payload}, 1)
			sub.Proven = false
			if sub.Reason == "" {
				sub.Reason = "find " + a + " runs " + payload[0] + " on paths matched at runtime"
			}
			if _, destructive := neverPersistPrograms[programBase(payload[0])]; destructive {
				if sub.Consequence < ConsequenceLocalDestructive {
					sub.Consequence = ConsequenceLocalDestructive
				}
			}
			e = joinEffects([]Effect{e, sub})
		}
	}
	return e, true
}

// semDeriveXargs classifies xargs: the command template is in argv
// (provable, derived recursively) but the ARGUMENTS arrive via stdin —
// the invocation is unprovable as a whole.
func semDeriveXargs(argv []string) (Effect, bool) {
	// xargs' own options end at the first non-option token — everything
	// after it is the command template VERBATIM (its flags are not
	// xargs' flags), so a generic getopt pass cannot parse this.
	args := argv[1:]
	i := 0
	for ; i < len(args); i++ {
		a := args[i]
		if a == "-" || !strings.HasPrefix(a, "-") {
			break
		}
		if strings.Contains(a, "=") && strings.HasPrefix(a, "--") {
			continue // --opt=value forms are self-contained
		}
		switch a {
		case "-n", "-P", "-L", "-l", "-I", "-s", "-E", "-e", "-d", "-a",
			"--max-args", "--max-procs", "--max-lines", "--replace",
			"--delimiter", "--eof", "--max-chars", "--process-slot-var",
			"--arg-file":
			i++ // value-taking: consume the next token
		case "-0", "-r", "-p", "-t", "-x", "-o",
			"--null", "--no-run-if-empty", "--interactive", "--verbose",
			"--exit", "--show-limits", "--open-tty", "--parallel":
			// boolean flags
		default:
			return Effect{}, false // unknown xargs flag: do not guess
		}
	}
	if i >= len(args) {
		// xargs with no command runs echo — harmless.
		return Effect{
			Proven: true, Consequence: ConsequenceConfined,
			Reason: "xargs with no command template echoes its input",
		}, true
	}
	command := args[i:]
	sub := deriveStepRec(ExecStep{Argv: command}, 1)
	sub.Proven = false
	if sub.Reason == "" {
		sub.Reason = "xargs feeds runtime stdin data to " + command[0]
	}
	if _, destructive := neverPersistPrograms[programBase(command[0])]; destructive {
		if sub.Consequence < ConsequenceLocalDestructive {
			sub.Consequence = ConsequenceLocalDestructive
		}
	}
	return sub, true
}

// semDeriveOsascript classifies osascript: it drives other applications
// via Apple Events under loom's TCC identity — a standing indicator,
// content unprovable.
func semDeriveOsascript(argv []string) (Effect, bool) {
	return Effect{
		Proven:     false,
		Reason:     "osascript executes AppleScript that drives other applications",
		GUIOpen:    true,
		Indicators: []string{"osascript drives other applications via Apple Events (TCC-attributed to loom)"},
	}, true
}

// semDeriveLaunchctl classifies launchctl: it manages launch
// agents/daemons — the macOS persistence mechanism.
func semDeriveLaunchctl(argv []string) (Effect, bool) {
	return Effect{
		Proven:      true,
		Consequence: ConsequenceLocalDestructive,
		Reason:      "launchctl manages launch agents/daemons (persistence mechanism)",
		Indicators:  []string{"launchctl installs or controls persistent background services"},
	}, true
}

// semDeriveCrontab classifies crontab: editing cron jobs is a
// persistence mechanism.
func semDeriveCrontab(argv []string) (Effect, bool) {
	for _, arg := range argv[1:] {
		if arg == "-l" || arg == "--list" {
			return Effect{
				Proven: true, Consequence: ConsequenceConfined,
				Reason: "crontab -l lists the current crontab",
			}, true
		}
	}
	return Effect{
		Proven:      true,
		Consequence: ConsequenceLocalDestructive,
		Reason:      "crontab edits scheduled jobs (persistence mechanism)",
		Indicators:  []string{"crontab installs commands that run on a schedule, outside any sandbox"},
	}, true
}

// npmSubcommands maps package-manager subcommands to their effect.
var npmSubcommands = map[string]Effect{
	"publish":   {Proven: true, Consequence: ConsequenceSharedState, Network: HostSet{Any: true}, Reason: "publishes a package to the registry"},
	"install":   {Proven: true, Consequence: ConsequenceConfined, Network: HostSet{Any: true}, Reason: "installs dependencies from the registry"},
	"i":         {Proven: true, Consequence: ConsequenceConfined, Network: HostSet{Any: true}, Reason: "installs dependencies from the registry"},
	"add":       {Proven: true, Consequence: ConsequenceConfined, Network: HostSet{Any: true}, Reason: "installs dependencies from the registry"},
	"update":    {Proven: true, Consequence: ConsequenceConfined, Network: HostSet{Any: true}, Reason: "updates dependencies from the registry"},
	"exec":      {Proven: false, Reason: "executes package code downloaded at runtime"},
	"dlx":       {Proven: false, Reason: "executes package code downloaded at runtime"},
	"uninstall": {Proven: true, Consequence: ConsequenceConfined, Reason: "removes installed dependencies"},
}

// semDeriveNpm classifies npm/pnpm/yarn invocations by subcommand.
func semDeriveNpm(argv []string) (Effect, bool) {
	if len(argv) < 2 {
		return Effect{Proven: true, Consequence: ConsequenceConfined, Reason: argv[0]}, true
	}
	sub := argv[1]
	if strings.HasPrefix(sub, "-") {
		return Effect{
			Proven: true, Consequence: ConsequenceConfined,
			Reason: programBase(argv[0]) + " " + sub + " prints information",
		}, true
	}
	if e, ok := npmSubcommands[sub]; ok {
		return e, true
	}
	// run/test/build and friends execute project scripts — workspace
	// code, sandbox-confined.
	return Effect{
		Proven: true, Consequence: ConsequenceConfined,
		Reason: programBase(argv[0]) + " " + sub,
	}, true
}

// semDeriveGo classifies go tool invocations: module-fetching forms need
// network; build/test/run are workspace code under the sandbox.
func semDeriveGo(argv []string) (Effect, bool) {
	if len(argv) < 2 {
		return Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "go"}, true
	}
	switch argv[1] {
	case "mod", "get", "install":
		return Effect{
			Proven: true, Consequence: ConsequenceConfined,
			Network: HostSet{Any: true},
			Reason:  "go " + argv[1] + " fetches modules from the network",
		}, true
	}
	return Effect{
		Proven: true, Consequence: ConsequenceConfined,
		Reason: "go " + argv[1],
	}, true
}

// semDeriveDocker classifies docker invocations: push mutates shared
// registry state; pull needs network; run/exec execute container code
// (unprovable content, but the container boundary applies).
func semDeriveDocker(argv []string) (Effect, bool) {
	if len(argv) < 2 {
		return Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "docker"}, true
	}
	switch argv[1] {
	case "push":
		return Effect{
			Proven: true, Consequence: ConsequenceSharedState,
			Network: HostSet{Any: true}, Reason: "docker push publishes an image to a registry",
		}, true
	case "pull", "login", "logout", "search":
		return Effect{
			Proven: true, Consequence: ConsequenceConfined,
			Network: HostSet{Any: true}, Reason: "docker " + argv[1],
		}, true
	case "run", "exec", "create":
		// Container escapes: host mounts, privileged mode, or host
		// networking dissolve the container boundary.
		for _, arg := range argv[2:] {
			if arg == "--privileged" || arg == "--net=host" || arg == "--network=host" ||
				arg == "-v" || arg == "--volume" || strings.HasPrefix(arg, "--volume=") ||
				strings.HasPrefix(arg, "--mount") {
				return Effect{
					Proven:      false,
					Reason:      "docker " + argv[1] + " with host mounts/privileges dissolves the container boundary",
					Indicators:  []string{"docker " + argv[1] + " escapes the container boundary (mounts/privileged/host net)"},
					Consequence: ConsequenceLocalDestructive,
				}, true
			}
		}
		return Effect{Proven: false, Reason: "docker " + argv[1] + " executes container content not statically analyzable"}, true
	}
	return Effect{
		Proven: true, Consequence: ConsequenceConfined,
		Reason: "docker " + argv[1],
	}, true
}

// semDeriveKubectl classifies kubectl invocations: reads are network
// confined; apply mutates cluster state; delete destroys it.
func semDeriveKubectl(argv []string) (Effect, bool) {
	if len(argv) < 2 {
		return Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "kubectl"}, true
	}
	switch argv[1] {
	case "delete", "drain", "cordon", "taint":
		return Effect{
			Proven: true, Consequence: ConsequenceSharedDestructive,
			Network: HostSet{Any: true},
			Reason:  "kubectl " + argv[1] + " destroys shared cluster state",
		}, true
	case "apply", "create", "scale", "rollout", "patch", "replace", "edit", "label", "annotate", "expose", "autoscale":
		return Effect{
			Proven: true, Consequence: ConsequenceSharedState,
			Network: HostSet{Any: true},
			Reason:  "kubectl " + argv[1] + " mutates shared cluster state",
		}, true
	case "exec", "cp", "port-forward", "attach", "proxy":
		// Remote execution / arbitrary data movement inside the
		// cluster: the payload is not statically analyzable.
		return Effect{
			Proven:     false,
			Network:    HostSet{Any: true},
			Reason:     "kubectl " + argv[1] + " executes or tunnels into the cluster (payload not analyzable)",
			Indicators: []string{"kubectl " + argv[1] + " runs commands or copies data inside the cluster, beyond the sandbox"},
		}, true
	}
	return Effect{
		Proven: true, Consequence: ConsequenceConfined,
		Network: HostSet{Any: true}, Reason: "kubectl " + argv[1],
	}, true
}
