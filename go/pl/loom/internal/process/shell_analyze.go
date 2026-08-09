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

package process

import (
	"strings"

	"mvdan.cc/sh/v3/syntax"
)

// ShellCommand is one simple command extracted from a shell script. A nil
// Argv marks a command whose words contain dynamic expansions (variables,
// substitutions, arithmetic): the program cannot be proven statically.
type ShellCommand struct {
	Argv []string
}

// PipeEdge records one "producer | consumer" pair of a pipeline. Producer
// and Consumer are program basenames ("" when that side did not resolve
// statically); ConsumerArgv keeps the consumer's full static argv so
// pipe-into-interpreter detection can distinguish `| sh` (executes stdin)
// from `| python3 analyze.py` (runs a fixed script).
type PipeEdge struct {
	Producer     string
	Consumer     string
	ConsumerArgv []string
}

// ShellAnalysis is the static classification of a `sh -c` script, built by
// walking the parsed AST once. It serves three distinct policy uses:
//
//   - danger screening: every literal command (including ones nested in
//     substitutions and subshells) is collected in Commands, plus the
//     pipeline and write-redirect shapes a naive argv scan cannot see;
//   - whitelist evaluation: when Static is true, every command's argv is
//     exact and may be matched against argv-prefix rules individually;
//   - memory eligibility: only a Static analysis with no DynamicWrites is
//     a honest basis for a standing approval.
//
// Execution is unaffected by the analysis — the script still runs through
// the shell inside the sandbox; the analysis only feeds classification.
type ShellAnalysis struct {
	// Commands lists every simple command in the script, including those
	// nested inside command substitutions and subshells.
	Commands []ShellCommand
	// Static reports that the whole script resolved statically: no
	// variable expansions, substitutions, control flow, subshells,
	// background jobs, env-assignment prefixes, or heredocs.
	Static bool
	// Pipes lists the producer→consumer pairs of every pipeline.
	Pipes []PipeEdge
	// WriteRedirects lists the static targets of file-writing redirects
	// (>, >>, >|, &>, &>>, >&file).
	WriteRedirects []string
	// DynamicWrites reports that a file-writing redirect had a dynamic
	// target (e.g. > "$out").
	DynamicWrites bool
}

// AnalyzeShellArgv classifies a ["sh", "-c", script]-form argv. ok=false
// when argv is not a shell -c invocation or the script fails to parse.
func AnalyzeShellArgv(argv []string) (ShellAnalysis, bool) {
	if len(argv) != 3 || !IsShellProgram(argv[0]) || argv[1] != "-c" {
		return ShellAnalysis{}, false
	}
	return AnalyzeShellScript(argv[2])
}

// AnalyzeShellScript parses and classifies a shell script. ok=false on a
// parse error or when the script contains no command at all (e.g. only
// assignments) — callers keep their conservative fallback in that case.
func AnalyzeShellScript(script string) (ShellAnalysis, bool) {
	file, err := syntax.NewParser().Parse(strings.NewReader(script), "")
	if err != nil {
		return ShellAnalysis{}, false
	}
	a := &shellAnalyzer{analysis: ShellAnalysis{Static: true}}
	syntax.Walk(file, a.visit)
	if len(a.analysis.Commands) == 0 {
		return ShellAnalysis{}, false
	}
	return a.analysis, true
}

// shellAnalyzer accumulates the classification during the AST walk.
type shellAnalyzer struct {
	analysis ShellAnalysis
}

func (a *shellAnalyzer) visit(n syntax.Node) bool {
	switch node := n.(type) {
	case *syntax.CallExpr:
		a.addCall(node)
	case *syntax.BinaryCmd:
		if node.Op == syntax.Pipe || node.Op == syntax.PipeAll {
			a.addPipe(node)
		}
	case *syntax.Redirect:
		a.addRedirect(node)
	case *syntax.Stmt:
		if node.Background || node.Negated || node.Coprocess {
			a.analysis.Static = false
		}
	case *syntax.Subshell, *syntax.Block, *syntax.IfClause, *syntax.WhileClause,
		*syntax.ForClause, *syntax.CaseClause, *syntax.TimeClause,
		*syntax.TestClause, *syntax.DeclClause, *syntax.LetClause, *syntax.ArithmCmd,
		*syntax.FuncDecl:
		// Control flow, grouping, and declarations hide execution shape
		// from a prefix matcher; literals inside are still collected for
		// the danger screen because Walk descends into them.
		a.analysis.Static = false
	}
	return true
}

// addCall resolves a simple command's words. A pure env-assignment
// statement (FOO=bar with no command) is not a command at all.
func (a *shellAnalyzer) addCall(c *syntax.CallExpr) {
	if len(c.Assigns) > 0 {
		a.analysis.Static = false
	}
	if len(c.Args) == 0 {
		return
	}
	argv, static := resolveWords(c.Args)
	if !static {
		a.analysis.Static = false
	}
	a.analysis.Commands = append(a.analysis.Commands, ShellCommand{Argv: argv})
}

// addPipe records the producer→consumer pair of a pipeline. Nested
// pipelines (a | b | c) decompose into per-edge pairs: the producer of the
// outer edge is the LAST command of its left side.
func (a *shellAnalyzer) addPipe(bc *syntax.BinaryCmd) {
	edge := PipeEdge{Producer: callProgram(lastCall(bc.X))}
	if first := firstCall(bc.Y); first != nil {
		argv, static := resolveWords(first.Args)
		if static && len(argv) > 0 {
			edge.Consumer = programBaseName(argv[0])
			edge.ConsumerArgv = argv
		}
	}
	// Walk visits outer pipe nodes before their nested left side, so
	// prepending keeps the edges in data-flow order (a|b|c records
	// a→b before b→c).
	a.analysis.Pipes = append([]PipeEdge{edge}, a.analysis.Pipes...)
}

// addRedirect classifies redirections. Heredocs make the script non-static
// (their body is arbitrary content); file-writing redirects record their
// target for the danger screen; input redirects and fd duplications are
// behavior-neutral for classification.
func (a *shellAnalyzer) addRedirect(r *syntax.Redirect) {
	switch r.Op {
	case syntax.Hdoc, syntax.DashHdoc, syntax.WordHdoc:
		a.analysis.Static = false
	case syntax.RdrOut, syntax.AppOut, syntax.RdrAll, syntax.AppAll, syntax.ClbOut:
		a.addWriteTarget(r.Word)
	case syntax.DplOut:
		// >&2 duplicates a descriptor; >&file writes a file.
		if r.Word != nil && !isFdWord(r.Word) {
			a.addWriteTarget(r.Word)
		}
	}
}

func (a *shellAnalyzer) addWriteTarget(w *syntax.Word) {
	target, ok := resolveWord(w)
	if !ok {
		a.analysis.DynamicWrites = true
		a.analysis.Static = false
		return
	}
	a.analysis.WriteRedirects = append(a.analysis.WriteRedirects, target)
}

// firstCall returns the first simple command of a command node, descending
// through statement wrappers and binary compositions.
func firstCall(n syntax.Node) *syntax.CallExpr {
	switch node := n.(type) {
	case *syntax.Stmt:
		return firstCall(node.Cmd)
	case *syntax.BinaryCmd:
		if c := firstCall(node.X); c != nil {
			return c
		}
		return firstCall(node.Y)
	case *syntax.CallExpr:
		return node
	}
	return nil
}

// lastCall mirrors firstCall for the rightmost command.
func lastCall(n syntax.Node) *syntax.CallExpr {
	switch node := n.(type) {
	case *syntax.Stmt:
		return lastCall(node.Cmd)
	case *syntax.BinaryCmd:
		if c := lastCall(node.Y); c != nil {
			return c
		}
		return lastCall(node.X)
	case *syntax.CallExpr:
		return node
	}
	return nil
}

// callProgram resolves a call's program basename ("" when dynamic).
func callProgram(c *syntax.CallExpr) string {
	if c == nil || len(c.Args) == 0 {
		return ""
	}
	program, ok := resolveWord(c.Args[0])
	if !ok {
		return ""
	}
	return programBaseName(program)
}

func programBaseName(program string) string {
	if idx := strings.LastIndexAny(program, `/\`); idx >= 0 {
		program = program[idx+1:]
	}
	return strings.ToLower(program)
}

// isFdWord reports whether a redirect word is a bare file descriptor
// number (as in >&2), not a file path.
func isFdWord(w *syntax.Word) bool {
	s, ok := resolveWord(w)
	if !ok || s == "" {
		return false
	}
	for _, r := range s {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

// resolveWords resolves every word of a command; ok=false when any word
// contains a dynamic part.
func resolveWords(words []*syntax.Word) ([]string, bool) {
	out := make([]string, 0, len(words))
	for _, w := range words {
		s, ok := resolveWord(w)
		if !ok {
			return nil, false
		}
		out = append(out, s)
	}
	return out, true
}

// resolveWord renders a word to its static text: literals, single quotes,
// and double quotes containing only literals. Any expansion (parameter,
// command, arithmetic, process substitution, glob braces) fails the word.
// Globs inside literals are kept verbatim — they may expand at execution
// time but can never change the program or the token count, so prefix
// classification on the resolved argv stays sound.
func resolveWord(w *syntax.Word) (string, bool) {
	if w == nil {
		return "", false
	}
	var b strings.Builder
	for _, part := range w.Parts {
		switch p := part.(type) {
		case *syntax.Lit:
			b.WriteString(p.Value)
		case *syntax.SglQuoted:
			b.WriteString(p.Value)
		case *syntax.DblQuoted:
			for _, inner := range p.Parts {
				lit, ok := inner.(*syntax.Lit)
				if !ok {
					return "", false
				}
				b.WriteString(lit.Value)
			}
		default:
			return "", false
		}
	}
	return b.String(), true
}

// UnwrapSimpleShell rewrites ["sh", "-c", script] (or another supported
// shell) into the plain argv the script denotes — but only when the script
// is provably ONE static command: no pipes, redirections, sequencing,
// substitutions, control flow, or env-assignment prefixes. Execution is
// unaffected — the command still runs through the shell; the unwrapped
// argv only feeds risk classification, danger screening, and argv-prefix
// rule matching.
func UnwrapSimpleShell(argv []string) ([]string, bool) {
	analysis, ok := AnalyzeShellArgv(argv)
	if !ok || !analysis.Static {
		return nil, false
	}
	if len(analysis.Commands) != 1 || len(analysis.Pipes) > 0 ||
		len(analysis.WriteRedirects) > 0 || analysis.DynamicWrites {
		return nil, false
	}
	cmd := analysis.Commands[0].Argv
	if len(cmd) == 0 {
		return nil, false
	}
	return cmd, true
}
