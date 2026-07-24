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
	"encoding/json"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// diagnostic is the normalized, linter-agnostic finding returned to the model.
type diagnostic struct {
	Path     string `json:"path"`
	Line     int    `json:"line"`
	Column   int    `json:"column,omitempty"`
	Severity string `json:"severity"`
	Code     string `json:"code,omitempty"`
	Message  string `json:"message"`
	Source   string `json:"source"`
}

// --- golangci-lint JSON ---

type golangciReport struct {
	Issues []struct {
		FromLinter string `json:"FromLinter"`
		Text       string `json:"Text"`
		Severity   string `json:"Severity"`
		Pos        struct {
			Filename string `json:"Filename"`
			Line     int    `json:"Line"`
			Column   int    `json:"Column"`
		} `json:"Pos"`
	} `json:"Issues"`
}

func parseGolangCIOutput(stdout []byte) ([]diagnostic, error) {
	if len(strings.TrimSpace(string(stdout))) == 0 {
		// Older golangci-lint versions emit no JSON at all when there are no
		// issues; treat empty output as a clean result.
		return []diagnostic{}, nil
	}
	var report golangciReport
	if err := json.Unmarshal(stdout, &report); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to parse golangci-lint JSON output", domain.WithCause(err))
	}
	out := make([]diagnostic, 0, len(report.Issues))
	for _, issue := range report.Issues {
		severity := strings.ToLower(issue.Severity)
		if severity != "error" {
			severity = "warning"
		}
		out = append(out, diagnostic{
			Path:     issue.Pos.Filename,
			Line:     issue.Pos.Line,
			Column:   issue.Pos.Column,
			Severity: severity,
			Message:  strings.TrimSpace(issue.Text),
			Source:   issue.FromLinter,
		})
	}
	return out, nil
}

// --- go vet text (stderr): file:line:col: message ---

var vetLinePattern = regexp.MustCompile(`^([^\s][^:]*):(\d+):(\d+):\s+(.+)$`)

func parseGoVetOutput(stderr []byte) []diagnostic {
	var out []diagnostic
	for _, line := range strings.Split(string(stderr), "\n") {
		m := vetLinePattern.FindStringSubmatch(strings.TrimRight(line, "\r"))
		if m == nil {
			continue
		}
		out = append(out, diagnostic{
			Path:     filepath.ToSlash(m[1]),
			Line:     atoi(m[2]),
			Column:   atoi(m[3]),
			Severity: "error",
			Message:  strings.TrimSpace(m[4]),
			Source:   "go vet",
		})
	}
	return out
}

// --- eslint JSON ---

type eslintFileReport struct {
	FilePath string `json:"filePath"`
	Messages []struct {
		RuleID   string `json:"ruleId"`
		Severity int    `json:"severity"`
		Message  string `json:"message"`
		Line     int    `json:"line"`
		Column   int    `json:"column"`
	} `json:"messages"`
}

func parseESLintOutput(stdout []byte) ([]diagnostic, error) {
	if len(strings.TrimSpace(string(stdout))) == 0 {
		return []diagnostic{}, nil
	}
	var reports []eslintFileReport
	if err := json.Unmarshal(stdout, &reports); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to parse eslint JSON output", domain.WithCause(err))
	}
	var out []diagnostic
	for _, file := range reports {
		for _, msg := range file.Messages {
			severity := "warning"
			if msg.Severity >= 2 {
				severity = "error"
			}
			out = append(out, diagnostic{
				Path:     file.FilePath,
				Line:     msg.Line,
				Column:   msg.Column,
				Severity: severity,
				Code:     msg.RuleID,
				Message:  strings.TrimSpace(msg.Message),
				Source:   "eslint",
			})
		}
	}
	return out, nil
}

// --- ruff JSON ---

type ruffFinding struct {
	Code     string `json:"code"`
	Message  string `json:"message"`
	Filename string `json:"filename"`
	Location struct {
		Row    int `json:"row"`
		Column int `json:"column"`
	} `json:"location"`
}

func parseRuffOutput(stdout []byte) ([]diagnostic, error) {
	if len(strings.TrimSpace(string(stdout))) == 0 {
		return []diagnostic{}, nil
	}
	var findings []ruffFinding
	if err := json.Unmarshal(stdout, &findings); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to parse ruff JSON output", domain.WithCause(err))
	}
	out := make([]diagnostic, 0, len(findings))
	for _, f := range findings {
		out = append(out, diagnostic{
			Path:     f.Filename,
			Line:     f.Location.Row,
			Column:   f.Location.Column,
			Severity: "warning",
			Code:     f.Code,
			Message:  strings.TrimSpace(f.Message),
			Source:   "ruff",
		})
	}
	return out, nil
}

// --- clang-tidy text (stdout): file:line:col: severity: message [check] ---

var clangTidyLinePattern = regexp.MustCompile(`^([^:]+):(\d+):(\d+):\s+(error|warning|note|remark):\s+(.*?)(\s+\[([^\]]+)\])?$`)

func parseClangTidyOutput(stdout []byte) []diagnostic {
	var out []diagnostic
	for _, line := range strings.Split(string(stdout), "\n") {
		m := clangTidyLinePattern.FindStringSubmatch(strings.TrimRight(line, "\r"))
		if m == nil {
			continue
		}
		severity := m[4]
		if severity == "note" || severity == "remark" {
			// Supplementary context lines duplicate their parent diagnostic.
			continue
		}
		out = append(out, diagnostic{
			Path:     filepath.ToSlash(m[1]),
			Line:     atoi(m[2]),
			Column:   atoi(m[3]),
			Severity: severity,
			Code:     m[7], // bracketed check name, e.g. [readability-braces-around-statements]
			Message:  strings.TrimSpace(m[5]),
			Source:   "clang-tidy",
		})
	}
	return out
}

// --- normalization ---

// normalizeDiagnostics maps linter-reported paths to workspace-relative
// display paths, applies the severity filter, sorts deterministically and
// caps the result. Paths that cannot be related back to the workspace are
// kept as-is so the model still sees them.
func normalizeDiagnostics(diags []diagnostic, plan enginePlan, wsRoot, severity string, max int) ([]diagnostic, bool) {
	filtered := make([]diagnostic, 0, len(diags))
	for _, d := range diags {
		if !severityAllowed(severity, d.Severity) {
			continue
		}
		d.Path = displayDiagnosticPath(wsRoot, plan.ProjectRoot, d.Path)
		if d.Severity == "" {
			d.Severity = "warning"
		}
		filtered = append(filtered, d)
	}
	sort.SliceStable(filtered, func(i, j int) bool {
		a, b := filtered[i], filtered[j]
		if a.Path != b.Path {
			return a.Path < b.Path
		}
		if a.Line != b.Line {
			return a.Line < b.Line
		}
		if a.Column != b.Column {
			return a.Column < b.Column
		}
		return a.Message < b.Message
	})
	truncated := false
	if len(filtered) > max {
		filtered = filtered[:max]
		truncated = true
	}
	if filtered == nil {
		filtered = []diagnostic{}
	}
	return filtered, truncated
}

func severityAllowed(filter, severity string) bool {
	switch filter {
	case "", "all":
		return true
	case "error":
		return severity == "error"
	case "warning":
		return severity == "error" || severity == "warning"
	default:
		return true
	}
}

// displayDiagnosticPath converts a linter-reported path (relative to the
// process cwd or absolute) into a workspace-relative display path.
func displayDiagnosticPath(wsRoot, cwd, p string) string {
	if p == "" {
		return p
	}
	abs := p
	if !filepath.IsAbs(abs) {
		abs = filepath.Join(cwd, abs)
	}
	abs = filepath.Clean(abs)
	if rel, err := filepath.Rel(wsRoot, abs); err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return displayPath(rel)
	}
	return filepath.ToSlash(p)
}

func atoi(s string) int {
	n, _ := strconv.Atoi(s)
	return n
}
