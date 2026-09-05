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

// ExecPlan is the normalized intermediate representation every exec-shape
// call is lowered to before effect derivation. All shell-form knowledge
// lives HERE and only here: -c flag variants, heredocs, pipes, wrapper
// programs, dynamic words. Downstream layers (semantic tables,
// indicators, package binding, memory derivation) never parse shell
// syntax again — the drift class of bugs where four hand-rolled parsers
// each understood a different subset of shell is designed out.
package permission

import (
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// ExecStep is one execution unit of a plan: a simple command with its
// stdin shape. A nil Argv marks a step whose words contain dynamic
// expansions — its program cannot be proven statically.
type ExecStep struct {
	Argv          []string
	Stdin         process.StdinSource
	Heredoc       string // static heredoc body (Stdin == StdinHeredoc)
	HeredocStatic bool
}

// ExecPlan is the normalized execution shape of one call.
type ExecPlan struct {
	// Steps are the plan's commands in execution order, already
	// flattened out of pipes, substitutions, subshells, and (for shell
	// consumers) heredoc bodies.
	Steps []ExecStep
	// Unanalyzable explains why the plan is incomplete: a script that
	// failed to parse, an unanalyzable shell invocation form, dynamic
	// words. Empty means every step's argv is exact. Consumers must
	// treat a non-empty value as "unproven", NEVER as "safe".
	Unanalyzable string
	// WriteRedirects lists the static targets of the script's
	// file-writing redirects (they are effects of the plan as a whole,
	// not of any single step's argv).
	WriteRedirects []string
	// Pipes lists the producer→consumer edges of every pipeline
	// (cross-command indicator input).
	Pipes []process.PipeEdge
	// DynamicWrites reports a file-writing redirect with a dynamic
	// target.
	DynamicWrites bool
}

// maxHeredocDepth bounds recursive heredoc-body analysis so a
// self-embedding script cannot recurse the normalizer forever.
const maxHeredocDepth = 4

// NormalizeExec lowers a run_cmd/exec_session argv to its ExecPlan:
//
//   - a plain argv is a single step;
//   - a shell -c invocation (ANY flag form: -c, -lc, -ec, sh -cSCRIPT,
//     trailing arg0/args) is parsed and flattened;
//   - heredoc bodies feeding a SHELL are recursively analyzed as scripts
//     (the body IS code for that consumer); heredocs feeding anything
//     else stay attached to their step for the interpreter check;
//   - any other shell form (script file, bare stdin, --) degrades to a
//     single unanalyzable step.
func NormalizeExec(argv []string) ExecPlan {
	var plan ExecPlan
	if len(argv) == 0 {
		plan.Unanalyzable = "empty argv"
		return plan
	}
	script, isShellC := process.ShellScriptForm(argv)
	if !isShellC {
		if process.IsShellProgram(argv[0]) {
			plan.Steps = []ExecStep{{Argv: append([]string(nil), argv...)}}
			plan.Unanalyzable = "shell invocation is not an analyzable -c form (script file, bare stdin, or unrecognized options)"
			return plan
		}
		plan.Steps = []ExecStep{{Argv: append([]string(nil), argv...)}}
		return plan
	}
	return normalizeShellScript(script, argv, 0)
}

// normalizeShellScript parses one script and flattens it into a plan.
// raw is the outermost argv (used for the fallback step when the
// script cannot be analyzed); depth bounds heredoc recursion.
func normalizeShellScript(script string, raw []string, depth int) ExecPlan {
	var plan ExecPlan
	analysis, ok := process.AnalyzeShellScript(script)
	if !ok {
		plan.Steps = []ExecStep{{Argv: append([]string(nil), raw...)}}
		plan.Unanalyzable = "shell script failed to parse"
		return plan
	}
	plan.WriteRedirects = analysis.WriteRedirects
	plan.Pipes = analysis.Pipes
	plan.DynamicWrites = analysis.DynamicWrites
	if !analysis.Static {
		plan.Unanalyzable = "script contains dynamic constructs (variables, substitutions, control flow)"
	}
	for _, cmd := range analysis.Commands {
		step := ExecStep{
			Argv:          cmd.Argv,
			Stdin:         cmd.Stdin,
			Heredoc:       cmd.Heredoc,
			HeredocStatic: cmd.HeredocStatic,
		}
		if len(cmd.Argv) > 0 && cmd.Stdin == process.StdinHeredoc && cmd.HeredocStatic &&
			process.IsShellProgram(cmd.Argv[0]) && depth < maxHeredocDepth {
			// sh <<EOF ... EOF — the heredoc body is shell code:
			// analyze it recursively instead of treating the step as
			// an opaque interpreter-stdin execution.
			sub := normalizeShellScript(cmd.Heredoc, raw, depth+1)
			plan.Steps = append(plan.Steps, sub.Steps...)
			if plan.Unanalyzable == "" {
				plan.Unanalyzable = sub.Unanalyzable
			}
			plan.WriteRedirects = unionStrings(plan.WriteRedirects, sub.WriteRedirects)
			plan.Pipes = append(plan.Pipes, sub.Pipes...)
			plan.DynamicWrites = plan.DynamicWrites || sub.DynamicWrites
			continue
		}
		plan.Steps = append(plan.Steps, step)
	}
	return plan
}

// Argvs returns the step argvs that resolved statically (binding and
// memory matching consume these); dynamic steps are skipped — callers
// needing full-static matching check Unanalyzable first.
func (p ExecPlan) Argvs() [][]string {
	out := make([][]string, 0, len(p.Steps))
	for _, s := range p.Steps {
		if len(s.Argv) > 0 {
			out = append(out, s.Argv)
		}
	}
	return out
}
