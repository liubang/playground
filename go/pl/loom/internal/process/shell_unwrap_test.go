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
// Created: 2026/07/31

package process

import (
	"slices"
	"testing"
)

func TestUnwrapSimpleShell(t *testing.T) {
	unwrap := []struct {
		name string
		argv []string
		want []string
	}{
		{"plain command", []string{"sh", "-c", "ls -la"}, []string{"ls", "-la"}},
		{"bash", []string{"bash", "-c", "make build"}, []string{"make", "build"}},
		{"absolute shell path", []string{"/bin/zsh", "-c", "git status"}, []string{"git", "status"}},
		{"quoted argument", []string{"sh", "-c", `grep "hello world" a.txt`}, []string{"grep", "hello world", "a.txt"}},
		{"single quotes", []string{"sh", "-c", "echo 'a b'"}, []string{"echo", "a b"}},
		{"glob kept verbatim", []string{"sh", "-c", "ls *.go"}, []string{"ls", "*.go"}},
		{"extra whitespace", []string{"sh", "-c", "  mkdir   -p   .dsx_logs  "}, []string{"mkdir", "-p", ".dsx_logs"}},
	}
	for _, tt := range unwrap {
		t.Run(tt.name, func(t *testing.T) {
			got, ok := UnwrapSimpleShell(tt.argv)
			if !ok {
				t.Fatalf("UnwrapSimpleShell(%v) = not ok, want %v", tt.argv, tt.want)
			}
			if !slices.Equal(got, tt.want) {
				t.Fatalf("UnwrapSimpleShell(%v) = %v, want %v", tt.argv, got, tt.want)
			}
		})
	}

	reject := []struct {
		name string
		argv []string
	}{
		{"pipe", []string{"sh", "-c", "cat a | head"}},
		{"and chaining", []string{"sh", "-c", "mkdir x && echo done"}},
		{"semicolon", []string{"sh", "-c", "cd /tmp; ls"}},
		{"redirect", []string{"sh", "-c", "echo hi > f.txt"}},
		{"input redirect", []string{"sh", "-c", "cat < f.txt"}},
		{"command substitution", []string{"sh", "-c", "echo $(date)"}},
		{"backticks", []string{"sh", "-c", "echo `date`"}},
		{"variable expansion", []string{"sh", "-c", "echo $HOME"}},
		{"background", []string{"sh", "-c", "sleep 1 &"}},
		{"subshell", []string{"sh", "-c", "(cd /tmp)"}},
		{"escape", []string{"sh", "-c", `echo a\ b`}},
		{"newline", []string{"sh", "-c", "ls\npwd"}},
		{"env assignment prefix", []string{"sh", "-c", "FOO=bar make build"}},
		{"unterminated quote", []string{"sh", "-c", `echo "a`}},
		{"extra argv after script", []string{"sh", "-c", "ls", "extra"}},
		{"not -c form", []string{"sh", "-l", "-c", "ls"}},
		{"not a shell", []string{"python3", "-c", "print(1)"}},
		{"empty script", []string{"sh", "-c", "   "}},
	}
	for _, tt := range reject {
		t.Run("reject/"+tt.name, func(t *testing.T) {
			if got, ok := UnwrapSimpleShell(tt.argv); ok {
				t.Fatalf("UnwrapSimpleShell(%v) = %v, ok — want rejection", tt.argv, got)
			}
		})
	}
}

func TestEnvAllowlistNodeOptionsAndSkillPrefix(t *testing.T) {
	allowlist := makeEnvAllowlist(nil)
	for _, key := range []string{"PATH", "HOME", "NODE_OPTIONS", "SKILL_REGION", "SKILL_SCENE"} {
		if !allowedEnvKey(key, allowlist) {
			t.Errorf("allowedEnvKey(%q) = false, want true", key)
		}
	}
	// The deny list still wins inside the SKILL_ namespace, and unknown
	// keys stay dropped.
	for _, key := range []string{"SKILL_SECRET_SAUCE", "SKILL_API_TOKEN", "AWS_ACCESS_KEY_ID", "NOT_LISTED"} {
		if allowedEnvKey(key, allowlist) {
			t.Errorf("allowedEnvKey(%q) = true, want false", key)
		}
	}
}
