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

// StdinSource identifies what feeds a command's standard input. It
// matters because several programs EXECUTE their stdin: a heredoc or a
// pipe feeding an interpreter carries code, and the policy layer must
// either analyze that code or treat the command as unprovable.
type StdinSource int

const (
	// StdinNone is the inherited/terminal stdin — no program input.
	StdinNone StdinSource = iota
	// StdinPipe is a pipeline producer's output.
	StdinPipe
	// StdinHeredoc is a <<EOF / <<-EOF body (Heredoc holds the literal
	// body when HeredocStatic).
	StdinHeredoc
	// StdinWord is a <<< here-string.
	StdinWord
)

// ShellCommand is one simple command extracted from a shell script. A nil
// Argv marks a command whose words contain dynamic expansions (variables,
// substitutions, arithmetic): the program cannot be proven statically.
type ShellCommand struct {
	Argv []string
	// Stdin records what feeds the command's stdin (StdinNone when
	// nothing does).
	Stdin StdinSource
	// Heredoc is the literal heredoc body (Stdin == StdinHeredoc only).
	Heredoc string
	// HeredocStatic reports that the heredoc body resolved to a pure
	// literal (no expansions). A dynamic body is arbitrary content.
	HeredocStatic bool
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

// ShellAnalysis is the static classification of a shell script, built by
// walking the parsed AST once. It serves three distinct policy uses:
//
//   - effect derivation: every literal command (including ones nested in
//     substitutions and subshells) is collected in Commands with its
//     stdin shape, plus the pipeline and write-redirect shapes a naive
//     argv scan cannot see;
//   - package binding: when Static is true, every command's argv is
//     exact and may be matched against argv-prefix bindings
//     individually;
//   - memory eligibility: only a Static analysis with no DynamicWrites
//     and no code-carrying stdin is an honest basis for a standing
//     approval.
//
// Execution is unaffected by the analysis — the script still runs through
// the shell inside the sandbox; the analysis only feeds classification.
type ShellAnalysis struct {
	// Commands lists every simple command in the script, including those
	// nested inside command substitutions and subshells.
	Commands []ShellCommand
	// Static reports that the whole script resolved statically: no
	// variable expansions, substitutions, control flow, subshells,
	// background jobs, env-assignment prefixes, and every heredoc body
	// is a pure literal.
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

// AnalyzeShellScript parses and classifies a shell script. ok=false on a
// parse error or when the script contains no command at all (e.g. only
// assignments) — callers treat !ok as "unanalyzable", never as "safe".
func AnalyzeShellScript(script string) (ShellAnalysis, bool) {
	file, err := syntax.NewParser().Parse(strings.NewReader(script), "")
	if err != nil {
		return ShellAnalysis{}, false
	}
	a := &shellAnalyzer{
		analysis: ShellAnalysis{Static: true},
		stdinOf:  map[*syntax.CallExpr]StdinSource{},
	}
	syntax.Walk(file, a.visit)
	if len(a.analysis.Commands) == 0 {
		return ShellAnalysis{}, false
	}
	return a.analysis, true
}

// shellAnalyzer accumulates the classification during the AST walk.
type shellAnalyzer struct {
	analysis ShellAnalysis
	// stdinOf records pipe consumers discovered at BinaryCmd nodes
	// (parents), consulted when the consumer's own Stmt is visited
	// (children).
	stdinOf map[*syntax.CallExpr]StdinSource
}

func (a *shellAnalyzer) visit(n syntax.Node) bool {
	switch node := n.(type) {
	case *syntax.Stmt:
		if node.Background || node.Negated || node.Coprocess {
			a.analysis.Static = false
		}
		if call, ok := node.Cmd.(*syntax.CallExpr); ok {
			a.addCallStmt(node, call)
		}
	case *syntax.BinaryCmd:
		if node.Op == syntax.Pipe || node.Op == syntax.PipeAll {
			a.addPipe(node)
		}
	case *syntax.Redirect:
		a.addRedirect(node)
	case *syntax.Subshell, *syntax.Block, *syntax.IfClause, *syntax.WhileClause,
		*syntax.ForClause, *syntax.CaseClause, *syntax.TimeClause,
		*syntax.TestClause, *syntax.DeclClause, *syntax.LetClause, *syntax.ArithmCmd,
		*syntax.FuncDecl:
		// Control flow, grouping, and declarations hide execution shape
		// from a prefix matcher; literals inside are still collected for
		// the effect derivation because Walk descends into them.
		a.analysis.Static = false
	}
	return true
}

// addCallStmt resolves one simple command together with the stdin shape
// its redirects and pipe position give it. A pure env-assignment
// statement (FOO=bar with no command) is not a command at all.
func (a *shellAnalyzer) addCallStmt(stmt *syntax.Stmt, c *syntax.CallExpr) {
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
	cmd := ShellCommand{Argv: argv}
	if src, ok := a.stdinOf[c]; ok {
		cmd.Stdin = src
	}
	for _, r := range stmt.Redirs {
		switch r.Op {
		case syntax.Hdoc, syntax.DashHdoc:
			cmd.Stdin = StdinHeredoc
			// r.Word is the DELIMITER; the body is r.Hdoc (already
			// tab-stripped for <<-). A body with expansions is
			// arbitrary content.
			if r.Hdoc != nil {
				if body, ok := resolveWord(r.Hdoc); ok {
					cmd.Heredoc, cmd.HeredocStatic = body, true
				} else {
					a.analysis.Static = false
				}
			}
		case syntax.WordHdoc:
			cmd.Stdin = StdinWord
		}
	}
	a.analysis.Commands = append(a.analysis.Commands, cmd)
}

// addPipe records the producer→consumer pair of a pipeline. Nested
// pipelines (a | b | c) decompose into per-edge pairs: the producer of
// the outer edge is the LAST command of its left side.
func (a *shellAnalyzer) addPipe(bc *syntax.BinaryCmd) {
	edge := PipeEdge{Producer: callProgram(lastCall(bc.X))}
	if first := firstCall(bc.Y); first != nil {
		a.stdinOf[first] = StdinPipe
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

// addRedirect classifies redirections. File-writing redirects record
// their target for the effect derivation; input redirects, fd
// duplications, and heredocs are behavior-neutral here (heredocs are
// associated with their command in addCallStmt).
func (a *shellAnalyzer) addRedirect(r *syntax.Redirect) {
	switch r.Op {
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
// through statement wrappers, binary compositions, and grouping
// constructs (a pipe consumer written as `{ sh; }` or `( sh )` is still
// that interpreter — hiding it would blind the pipe-into-interpreter
// indicator).
func firstCall(n syntax.Node) *syntax.CallExpr {
	switch node := n.(type) {
	case *syntax.Stmt:
		return firstCall(node.Cmd)
	case *syntax.BinaryCmd:
		if c := firstCall(node.X); c != nil {
			return c
		}
		return firstCall(node.Y)
	case *syntax.Block:
		if len(node.Stmts) > 0 {
			return firstCall(node.Stmts[0])
		}
	case *syntax.Subshell:
		if len(node.Stmts) > 0 {
			return firstCall(node.Stmts[0])
		}
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
	case *syntax.Block:
		if len(node.Stmts) > 0 {
			return lastCall(node.Stmts[len(node.Stmts)-1])
		}
	case *syntax.Subshell:
		if len(node.Stmts) > 0 {
			return lastCall(node.Stmts[len(node.Stmts)-1])
		}
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
