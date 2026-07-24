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
// Created: 2026/07/24

package lint

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Linter names accepted by the tool (args.Linter == "auto" runs detection).
const (
	linterAuto      = "auto"
	linterGolangCI  = "golangci-lint"
	linterGoVet     = "go-vet"
	linterESLint    = "eslint"
	linterRuff      = "ruff"
	linterClangTidy = "clang-tidy"
)

// parserKind identifies the output decoder for a linter's process output.
type parserKind int

const (
	parseGolangCI parserKind = iota
	parseGoVet
	parseESLint
	parseRuff
	parseClangTidy
)

// enginePlan is a fully-resolved lint invocation: working directory, argv,
// environment and the parser for its output.
type enginePlan struct {
	Linter      string
	ProjectRoot string
	DisplayRoot string
	Argv        []string
	Env         map[string]string
	Parse       parserKind
}

// lookPath is a package-level indirection so tests can stub binary lookup.
var lookPath = exec.LookPath

// eslintConfigNames are the config markers eslint requires next to
// package.json for the eslint engine to engage.
var eslintConfigNames = []string{
	"eslint.config.js", "eslint.config.mjs", "eslint.config.cjs",
	".eslintrc", ".eslintrc.js", ".eslintrc.cjs",
	".eslintrc.json", ".eslintrc.yml", ".eslintrc.yaml",
}

// cFamilyExts are the file extensions clang-tidy can check directly.
var cFamilyExts = map[string]struct{}{
	".c": {}, ".cc": {}, ".cpp": {}, ".cxx": {}, ".c++": {},
	".h": {}, ".hh": {}, ".hpp": {}, ".hxx": {},
}

// detect walks from the target directory up to the workspace root looking
// for project markers. The nearest marker wins; within one directory the
// probe order is go → eslint → ruff → clang-tidy.
func (t *LintTool) detect(target pathResolution, args lintArgs) (enginePlan, error) {
	if args.Linter != "" && args.Linter != linterAuto {
		return t.planFor(target, args.Linter)
	}

	wsRoot := t.base.validator.Root()
	dir := target.Absolute
	if !target.Info.IsDir() {
		dir = filepath.Dir(dir)
	}

	for {
		if fileExists(filepath.Join(dir, "go.mod")) {
			return t.goPlan(dir, target, linterAuto)
		}
		if eslintConfigAt(dir) {
			bin := filepath.Join(dir, "node_modules", ".bin", "eslint")
			if fileExists(bin) {
				return enginePlan{
					Linter:      linterESLint,
					ProjectRoot: dir,
					DisplayRoot: t.displayRoot(dir),
					Argv:        []string{bin, "--format", "json", target.Absolute},
					Parse:       parseESLint,
				}, nil
			}
			return enginePlan{}, domain.NewError(domain.ErrUnavailable,
				fmt.Sprintf("eslint config found in %s but node_modules/.bin/eslint is missing; run npm install first", t.displayRoot(dir)))
		}
		if fileExists(filepath.Join(dir, "pyproject.toml")) || fileExists(filepath.Join(dir, "ruff.toml")) {
			bin, err := lookPath("ruff")
			if err == nil {
				return enginePlan{
					Linter:      linterRuff,
					ProjectRoot: dir,
					DisplayRoot: t.displayRoot(dir),
					Argv:        []string{bin, "check", "--output-format", "json", target.Absolute},
					Parse:       parseRuff,
				}, nil
			}
			return enginePlan{}, domain.NewError(domain.ErrUnavailable,
				fmt.Sprintf("python project markers found in %s but ruff is not on PATH", t.displayRoot(dir)))
		}
		if fileExists(filepath.Join(dir, "compile_commands.json")) && isCFamilyFile(target) {
			if bin, err := lookPath("clang-tidy"); err == nil {
				return enginePlan{
					Linter:      linterClangTidy,
					ProjectRoot: dir,
					DisplayRoot: t.displayRoot(dir),
					Argv:        []string{bin, "-p", dir, target.Absolute},
					Parse:       parseClangTidy,
				}, nil
			}
		}

		if dir == wsRoot {
			break
		}
		parent := filepath.Dir(dir)
		if parent == dir || !withinRoot(wsRoot, parent) {
			break
		}
		dir = parent
	}
	return enginePlan{}, noLinterDetected(target.Display)
}

// goPlan selects golangci-lint when on PATH, falling back to go vet. When
// forced names a concrete go linter only that one is considered. The check
// scope is the target's package subtree relative to the module root.
func (t *LintTool) goPlan(moduleDir string, target pathResolution, forced string) (enginePlan, error) {
	scope := target.Absolute
	if !target.Info.IsDir() {
		scope = filepath.Dir(scope)
	}
	rel, err := filepath.Rel(moduleDir, scope)
	if err != nil {
		rel = "."
	}
	pattern := "./..."
	if rel != "." {
		pattern = "./" + filepath.ToSlash(rel) + "/..."
	}
	env := map[string]string{
		// The seatbelt sandbox only allows writes inside the workspace and
		// the temp dir; point the build cache at a persistent temp location
		// and force fully offline module resolution.
		"GOCACHE": filepath.Join(os.TempDir(), "loom-gocache"),
		"GOPROXY": "off",
		"GOFLAGS": "-mod=readonly",
	}

	if forced != linterGoVet {
		if bin, err := lookPath("golangci-lint"); err == nil {
			return enginePlan{
				Linter:      linterGolangCI,
				ProjectRoot: moduleDir,
				DisplayRoot: t.displayRoot(moduleDir),
				Argv:        []string{bin, "run", "--out-format", "json", pattern},
				Env:         env,
				Parse:       parseGolangCI,
			}, nil
		}
		if forced == linterGolangCI {
			return enginePlan{}, unavailable(linterGolangCI)
		}
	}
	bin, err := lookPath("go")
	if err != nil {
		if forced == linterGoVet {
			return enginePlan{}, unavailable(linterGoVet)
		}
		return enginePlan{}, domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("go.mod found in %s but neither golangci-lint nor the go toolchain is on PATH", t.displayRoot(moduleDir)))
	}
	return enginePlan{
		Linter:      linterGoVet,
		ProjectRoot: moduleDir,
		DisplayRoot: t.displayRoot(moduleDir),
		Argv:        []string{bin, "vet", pattern},
		Env:         env,
		Parse:       parseGoVet,
	}, nil
}

// planFor builds a plan for an explicitly requested linter, locating the
// appropriate project root by walking up from the target.
func (t *LintTool) planFor(target pathResolution, linter string) (enginePlan, error) {
	startDir := target.Absolute
	if !target.Info.IsDir() {
		startDir = filepath.Dir(startDir)
	}

	switch linter {
	case linterGolangCI, linterGoVet:
		root, ok := t.findUp(startDir, "go.mod")
		if !ok {
			return enginePlan{}, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("linter %s requires a go.mod above %s", linter, target.Display))
		}
		return t.goPlan(root, target, linter)

	case linterESLint:
		dir := startDir
		for {
			if eslintConfigAt(dir) {
				bin := filepath.Join(dir, "node_modules", ".bin", "eslint")
				if !fileExists(bin) {
					return enginePlan{}, domain.NewError(domain.ErrUnavailable,
						"node_modules/.bin/eslint is missing; run npm install first")
				}
				return enginePlan{
					Linter:      linterESLint,
					ProjectRoot: dir,
					DisplayRoot: t.displayRoot(dir),
					Argv:        []string{bin, "--format", "json", target.Absolute},
					Parse:       parseESLint,
				}, nil
			}
			parent, ok := t.parentWithin(dir)
			if !ok {
				return enginePlan{}, domain.NewError(domain.ErrInvalidInput,
					fmt.Sprintf("linter eslint requires a package.json with eslint config above %s", target.Display))
			}
			dir = parent
		}

	case linterRuff:
		bin, err := lookPath("ruff")
		if err != nil {
			return enginePlan{}, unavailable(linter)
		}
		root := t.base.validator.Root()
		if dir, ok := t.findUp(startDir, "pyproject.toml"); ok {
			root = dir
		} else if dir, ok := t.findUp(startDir, "ruff.toml"); ok {
			root = dir
		}
		return enginePlan{
			Linter:      linterRuff,
			ProjectRoot: root,
			DisplayRoot: t.displayRoot(root),
			Argv:        []string{bin, "check", "--output-format", "json", target.Absolute},
			Parse:       parseRuff,
		}, nil

	case linterClangTidy:
		if !isCFamilyFile(target) {
			return enginePlan{}, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("clang-tidy requires a C/C++ file target; got %s", target.Display))
		}
		compileDB, ok := t.findUp(startDir, "compile_commands.json")
		if !ok {
			return enginePlan{}, domain.NewError(domain.ErrInvalidInput,
				"clang-tidy requires a compile_commands.json above the target (e.g. bazel run :refresh_compile_commands)")
		}
		bin, err := lookPath("clang-tidy")
		if err != nil {
			return enginePlan{}, unavailable(linter)
		}
		return enginePlan{
			Linter:      linterClangTidy,
			ProjectRoot: compileDB,
			DisplayRoot: t.displayRoot(compileDB),
			Argv:        []string{bin, "-p", compileDB, target.Absolute},
			Parse:       parseClangTidy,
		}, nil
	}
	return enginePlan{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unknown linter %q", linter))
}

// findUp walks from dir to the workspace root looking for a directory
// containing name, returning that directory.
func (t *LintTool) findUp(dir, name string) (string, bool) {
	for {
		if fileExists(filepath.Join(dir, name)) {
			return dir, true
		}
		parent, ok := t.parentWithin(dir)
		if !ok {
			return "", false
		}
		dir = parent
	}
}

// parentWithin returns the parent directory while it stays inside the
// workspace; ok=false when dir is the workspace root or escaping it.
func (t *LintTool) parentWithin(dir string) (string, bool) {
	wsRoot := t.base.validator.Root()
	if dir == wsRoot {
		return "", false
	}
	parent := filepath.Dir(dir)
	if parent == dir || !withinRoot(wsRoot, parent) {
		return "", false
	}
	return parent, true
}

func (t *LintTool) displayRoot(abs string) string {
	rel, err := filepath.Rel(t.base.validator.Root(), abs)
	if err != nil {
		return abs
	}
	return displayPath(rel)
}

func eslintConfigAt(dir string) bool {
	if !fileExists(filepath.Join(dir, "package.json")) {
		return false
	}
	for _, name := range eslintConfigNames {
		if fileExists(filepath.Join(dir, name)) {
			return true
		}
	}
	return false
}

func isCFamilyFile(target pathResolution) bool {
	if target.Info.IsDir() {
		return false
	}
	_, ok := cFamilyExts[strings.ToLower(filepath.Ext(target.Absolute))]
	return ok
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

func withinRoot(root, path string) bool {
	root = filepath.Clean(root)
	path = filepath.Clean(path)
	return path == root || strings.HasPrefix(path, root+string(filepath.Separator))
}

func unavailable(linter string) error {
	return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("%s is not installed or not on PATH", linter))
}

func noLinterDetected(display string) error {
	return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf(
		"no linter detected for %s: probed for go.mod, package.json with eslint config, pyproject.toml/ruff.toml, and compile_commands.json; install the matching linter or pass an explicit 'linter' argument",
		display))
}
