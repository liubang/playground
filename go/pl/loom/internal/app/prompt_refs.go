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
// Created: 2026/08/22

package app

import (
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// Prompt reference resolution: the WebUI composer offers @file and /skill
// completion; without a resolver those markers are plain text the model may
// or may not act on. References are resolved at submission time and their
// content is appended as an explicit context block, so the model receives it
// directly — no glob+read round-trip, no reliance on conventions.
//
// The block is delimited by LoomContextMark and persisted with the user
// message, so transcript and export stay faithful to what the model saw; the
// WebUI collapses it into a chip.

// LoomContextMark delimits the machine-generated context appended to a user
// prompt. Exported for the transcript renderers (the WebUI collapses it).
const LoomContextMark = "<loom-context>"

const (
	maxRefFiles      = 8         // per-submission cap on resolved @file references
	maxRefFileBytes  = 32 << 10  // per-file injection cap
	maxRefTotalBytes = 256 << 10 // total injection cap (files + skill)
	maxRefDirEntries = 100       // entry cap for a directory reference listing
	maxRefSkillBytes = 32 << 10  // SKILL.md injection cap
)

var (
	atRefRe    = regexp.MustCompile(`@([^\s@]+)`)
	skillRefRe = regexp.MustCompile(`^/([a-zA-Z0-9_-]+)`)
	// Trailing CJK/ASCII punctuation after an @ reference belongs to the
	// sentence, not the path.
	refTrailers = "，。；、,.;:!?！？）)]》」\"'"
)

// enrichPromptWithReferences resolves @file references and a leading /skill
// trigger in a submitted prompt against the workspace root and the skills
// catalog, appending resolved content in a loom-context block. Unresolvable
// references are left as plain text (never an error).
func enrichPromptWithReferences(b *Bootstrap, prompt string, logger *slog.Logger) string {
	var catalog *skill.AtomicCatalog
	if b.Skills != nil {
		catalog = b.Skills.Catalog
	}
	return resolvePromptRefs(b.WorkspaceRoot, catalog, prompt, logger)
}

// resolvePromptRefs is the dependency-narrow core (unit-testable without a
// full Bootstrap).
func resolvePromptRefs(root string, catalog *skill.AtomicCatalog, prompt string, logger *slog.Logger) string {
	// Canonicalize the root: WorkspaceRoot is canonical in production, but a
	// non-canonical root from a caller (e.g. the macOS /var → /private/var
	// symlink) would make every containment check misfire against the
	// Canonicalize'd file paths.
	if resolved, err := filepath.EvalSymlinks(root); err == nil {
		root = resolved
	}
	var sb strings.Builder
	total := 0
	files := 0

	// --- @file references (deduped; directories inject a one-level listing,
	// files their content) ---
	seen := map[string]bool{}
	for _, m := range atRefRe.FindAllStringSubmatch(prompt, -1) {
		tok := strings.TrimRight(m[1], refTrailers)
		if tok == "" || seen[tok] || files >= maxRefFiles {
			continue
		}
		seen[tok] = true
		block, size := resolveFileRef(root, tok)
		if block == "" {
			continue // missing, out-of-workspace, or unreadable: stays plain text
		}
		if total+size > maxRefTotalBytes {
			continue // oversized: skip this one, smaller refs may still fit
		}
		total += size
		files++
		sb.WriteString(block)
	}

	// --- leading-/ skill trigger ---
	skillBlock := ""
	if m := skillRefRe.FindStringSubmatch(prompt); m != nil && catalog != nil {
		if block, size := resolveSkillRef(catalog, m[1]); block != "" && total+size <= maxRefTotalBytes {
			skillBlock = block
		}
	}

	if files == 0 && skillBlock == "" {
		return prompt
	}
	if logger != nil {
		logger.Debug("prompt references resolved", "files", files, "skill", skillBlock != "")
	}

	var out strings.Builder
	out.WriteString(prompt)
	out.WriteString("\n\n")
	out.WriteString(LoomContextMark)
	out.WriteString("\n")
	if files > 0 {
		out.WriteString("The user referenced the following workspace paths via @mentions; their current contents are inlined below — use them directly, no need to read them again:\n\n")
		out.WriteString(sb.String())
	}
	if skillBlock != "" {
		out.WriteString(skillBlock)
	}
	out.WriteString("\n")
	out.WriteString("</loom-context>")
	return out.String()
}

// resolveFileRef validates tok against the workspace root (symlink-resolved
// containment) and renders the reference block. Returns "" for anything that
// is not a readable in-workspace file/dir.
func resolveFileRef(root, tok string) (string, int) {
	abs := tok
	if !filepath.IsAbs(abs) {
		abs = filepath.Join(root, filepath.FromSlash(tok))
	}
	abs = workspace.Canonicalize(abs)
	if abs != root && !strings.HasPrefix(abs, root+string(filepath.Separator)) {
		return "", 0
	}
	info, err := os.Stat(abs)
	if err != nil {
		return "", 0
	}
	// A reference to the workspace root itself would otherwise leak the
	// absolute path into the injected block.
	rel := "."
	if abs != root {
		rel = filepath.ToSlash(strings.TrimPrefix(abs, root+string(filepath.Separator)))
	}

	if info.IsDir() {
		entries, err := os.ReadDir(abs)
		if err != nil {
			return "", 0
		}
		var names strings.Builder
		n := 0
		for _, e := range entries {
			if strings.HasPrefix(e.Name(), ".") {
				continue
			}
			if n >= maxRefDirEntries {
				names.WriteString("- … (truncated)\n")
				break
			}
			name := e.Name()
			if e.IsDir() {
				name += "/"
			}
			names.WriteString("- " + name + "\n")
			n++
		}
		block := fmt.Sprintf("<directory path=\"%s\">\n%s</directory>\n\n", rel, names.String())
		return block, len(block)
	}

	if !info.Mode().IsRegular() {
		return "", 0
	}
	data, err := os.ReadFile(abs)
	if err != nil {
		return "", 0
	}
	if looksBinaryRef(data) {
		block := fmt.Sprintf("<file path=\"%s\" skipped=\"binary\"/>\n\n", rel)
		return block, len(block)
	}
	truncated := ""
	if len(data) > maxRefFileBytes {
		data = data[:maxRefFileBytes]
		truncated = fmt.Sprintf(" truncated=\"%dKB\"", maxRefFileBytes>>10)
	}
	block := fmt.Sprintf("<file path=\"%s\"%s>\n%s\n</file>\n\n", rel, truncated, string(data))
	return block, len(block)
}

// resolveSkillRef renders the /skill trigger block: the SKILL.md body is
// inlined so the instructions provably take effect this turn (versus the
// prompt-level "please read it" convention).
func resolveSkillRef(catalog *skill.AtomicCatalog, name string) (string, int) {
	s := catalog.Get().Find(name)
	if s == nil {
		return "", 0
	}
	data, err := os.ReadFile(s.Path)
	if err != nil {
		return "", 0
	}
	truncated := ""
	if len(data) > maxRefSkillBytes {
		data = data[:maxRefSkillBytes]
		truncated = fmt.Sprintf(" truncated=\"%dKB\"", maxRefSkillBytes>>10)
	}
	block := fmt.Sprintf("The user invoked skill %q with /%s; its full instructions follow and take effect this turn:\n\n<skill name=\"%s\"%s>\n%s\n</skill>\n",
		name, name, name, truncated, string(data))
	return block, len(block)
}

// looksBinaryRef sniffs the first 512 bytes for a NUL byte.
func looksBinaryRef(data []byte) bool {
	head := data
	if len(head) > 512 {
		head = head[:512]
	}
	return strings.IndexByte(string(head), 0) >= 0
}
