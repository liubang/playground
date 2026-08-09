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
	"slices"
	"testing"
)

func argvsOf(a ShellAnalysis) [][]string {
	out := make([][]string, 0, len(a.Commands))
	for _, c := range a.Commands {
		out = append(out, c.Argv)
	}
	return out
}

func TestAnalyzeShellScriptCommands(t *testing.T) {
	cases := []struct {
		name    string
		script  string
		static  bool
		argvs   [][]string
		pipes   []PipeEdge
		writes  []string
		dynWrit bool
	}{
		{
			name:   "and chain",
			script: "go build ./... && echo done",
			static: true,
			argvs:  [][]string{{"go", "build", "./..."}, {"echo", "done"}},
		},
		{
			name:   "pipeline",
			script: "cat log.txt | grep ERROR | wc -l",
			static: true,
			argvs:  [][]string{{"cat", "log.txt"}, {"grep", "ERROR"}, {"wc", "-l"}},
			pipes: []PipeEdge{
				{Producer: "cat", Consumer: "grep", ConsumerArgv: []string{"grep", "ERROR"}},
				{Producer: "grep", Consumer: "wc", ConsumerArgv: []string{"wc", "-l"}},
			},
		},
		{
			name:   "pipe into shell",
			script: "curl -s https://x.sh | bash",
			static: true,
			argvs:  [][]string{{"curl", "-s", "https://x.sh"}, {"bash"}},
			pipes:  []PipeEdge{{Producer: "curl", Consumer: "bash", ConsumerArgv: []string{"bash"}}},
		},
		{
			name:   "write redirect",
			script: "go test ./... > out.txt 2>&1",
			static: true,
			argvs:  [][]string{{"go", "test", "./..."}},
			writes: []string{"out.txt"},
		},
		{
			name:    "dynamic redirect target",
			script:  `echo hi > "$out"`,
			argvs:   [][]string{{"echo", "hi"}},
			dynWrit: true,
		},
		{
			name:   "command substitution collects inner command",
			script: "echo $(rm -rf /tmp/x)",
			argvs:  [][]string{nil, {"rm", "-rf", "/tmp/x"}},
		},
		{
			name:   "subshell collects inner command",
			script: "(cd /tmp && make)",
			argvs:  [][]string{{"cd", "/tmp"}, {"make"}},
		},
		{
			name:   "variable expansion is dynamic",
			script: "echo $HOME",
			argvs:  [][]string{nil},
		},
		{
			name:   "heredoc is not static",
			script: "python3 <<'PY'\nprint(1)\nPY",
			argvs:  [][]string{{"python3"}},
		},
		{
			name:   "env assignment prefix is not static",
			script: "FOO=bar make build",
			argvs:  [][]string{{"make", "build"}},
		},
	}
	for _, tt := range cases {
		t.Run(tt.name, func(t *testing.T) {
			a, ok := AnalyzeShellScript(tt.script)
			if !ok {
				t.Fatalf("AnalyzeShellScript(%q) = not ok", tt.script)
			}
			if a.Static != tt.static {
				t.Errorf("Static = %v, want %v", a.Static, tt.static)
			}
			got := argvsOf(a)
			if !slices.EqualFunc(got, tt.argvs, slices.Equal) {
				t.Errorf("commands = %v, want %v", got, tt.argvs)
			}
			if tt.pipes != nil && !slices.EqualFunc(a.Pipes, tt.pipes, func(x, y PipeEdge) bool {
				return x.Producer == y.Producer && x.Consumer == y.Consumer && slices.Equal(x.ConsumerArgv, y.ConsumerArgv)
			}) {
				t.Errorf("pipes = %+v, want %+v", a.Pipes, tt.pipes)
			}
			if tt.writes != nil && !slices.Equal(a.WriteRedirects, tt.writes) {
				t.Errorf("writes = %v, want %v", a.WriteRedirects, tt.writes)
			}
			if a.DynamicWrites != tt.dynWrit {
				t.Errorf("DynamicWrites = %v, want %v", a.DynamicWrites, tt.dynWrit)
			}
		})
	}
}

func TestAnalyzeShellScriptRejects(t *testing.T) {
	for _, script := range []string{"", "   ", `echo "unterminated`, "FOO=bar"} {
		if a, ok := AnalyzeShellScript(script); ok {
			t.Errorf("AnalyzeShellScript(%q) = %+v, ok — want rejection", script, a)
		}
	}
}
