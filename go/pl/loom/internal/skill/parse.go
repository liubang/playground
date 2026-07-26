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
// Created: 2026/07/26

package skill

import (
	"errors"
	"fmt"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"
)

// parseSkill parses one SKILL.md file body into a Skill. dir and path must
// already be absolute and symlink-resolved.
func parseSkill(dir, path string, data []byte, scope Scope) (*Skill, error) {
	name, description, err := parseFrontmatter(string(data))
	if err != nil {
		return nil, err
	}
	if name == "" {
		name = sanitizeSingleLine(filepath.Base(dir))
	}
	if name == "" {
		name = "skill"
	}
	if len([]rune(name)) > MaxNameLen {
		return nil, fmt.Errorf("invalid name: exceeds maximum length of %d characters", MaxNameLen)
	}
	if description == "" {
		return nil, errors.New("missing field `description`")
	}
	return &Skill{
		Name:        name,
		Description: description,
		Path:        path,
		Dir:         dir,
		Scope:       scope,
	}, nil
}

type skillFrontmatter struct {
	Name        string `yaml:"name"`
	Description string `yaml:"description"`
}

// parseFrontmatter extracts and parses the YAML frontmatter. On YAML failure
// it retries once with repaired bare scalars (see repairFrontmatter); a failed
// retry reports the ORIGINAL error, mirroring codex.
func parseFrontmatter(contents string) (name, description string, err error) {
	fm, ok := extractFrontmatter(contents)
	if !ok {
		return "", "", errors.New("missing YAML frontmatter delimited by ---")
	}
	parsed, err := decodeFrontmatter(fm)
	if err != nil {
		if repaired, changed := repairFrontmatter(fm); changed {
			if reparsed, rerr := decodeFrontmatter(repaired); rerr == nil {
				parsed = reparsed
			} else {
				return "", "", fmt.Errorf("invalid YAML: %w", err)
			}
		} else {
			return "", "", fmt.Errorf("invalid YAML: %w", err)
		}
	}
	return sanitizeSingleLine(parsed.Name), sanitizeSingleLine(parsed.Description), nil
}

func decodeFrontmatter(fm string) (skillFrontmatter, error) {
	var out skillFrontmatter
	if err := yaml.Unmarshal([]byte(fm), &out); err != nil {
		return skillFrontmatter{}, err
	}
	return out, nil
}

// extractFrontmatter returns the lines between the opening and closing ---
// delimiters. Delimiter lines may carry trailing whitespace (trimmed before
// comparison, like codex).
func extractFrontmatter(contents string) (string, bool) {
	lines := strings.Split(contents, "\n")
	if len(lines) == 0 || strings.TrimSpace(lines[0]) != "---" {
		return "", false
	}
	var body []string
	for _, line := range lines[1:] {
		if strings.TrimSpace(line) == "---" {
			if len(body) == 0 {
				return "", false
			}
			return strings.Join(body, "\n"), true
		}
		body = append(body, line)
	}
	return "", false
}

// repairFrontmatter quotes bare scalar values that break YAML parsing
// (e.g. `description: Build for AWS: ECS`). It preserves codex's three
// properties: already-quoted scalars are skipped, block scalar (| >) bodies
// are skipped, and only changed lines are rewritten.
func repairFrontmatter(fm string) (string, bool) {
	lines := strings.Split(fm, "\n")
	changed := false
	blockIndent := -1
	for i, line := range lines {
		indent := leadingSpaces(line)
		if blockIndent >= 0 {
			if strings.TrimSpace(line) == "" || indent > blockIndent {
				continue
			}
			blockIndent = -1
		}
		key, value, found := strings.Cut(line, ":")
		if !found || strings.TrimSpace(key) == "" || value == "" || (value[0] != ' ' && value[0] != '\t') {
			continue
		}
		trimmed := strings.TrimLeft(value, " \t")
		if trimmed == "" {
			continue
		}
		switch trimmed[0] {
		case '|', '>':
			blockIndent = indent
			continue
		case '\'', '"':
			continue
		}
		// A bare scalar that embeds ": " or starts with a YAML-significant
		// character is quoted whole (trailing comment preserved).
		if !strings.Contains(trimmed, ": ") && !strings.ContainsRune("[]{}@`&*!|>%", rune(trimmed[0])) {
			continue
		}
		scalar, comment := trimmed, ""
		if idx := strings.Index(trimmed, " #"); idx >= 0 {
			scalar = strings.TrimRight(trimmed[:idx], " \t")
			comment = trimmed[idx:]
		}
		if scalar == "" {
			continue
		}
		leading := value[:len(value)-len(trimmed)]
		lines[i] = key + ":" + leading + "'" + strings.ReplaceAll(scalar, "'", "''") + "'" + comment
		changed = true
	}
	if !changed {
		return fm, false
	}
	return strings.Join(lines, "\n"), true
}

func leadingSpaces(s string) int {
	n := 0
	for n < len(s) && s[n] == ' ' {
		n++
	}
	return n
}

// sanitizeSingleLine collapses all whitespace runs into single spaces.
func sanitizeSingleLine(raw string) string {
	return strings.Join(strings.Fields(raw), " ")
}
