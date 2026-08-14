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
// Created: 2026/08/14

package render

import "testing"

func TestCommandLineForDisplay(t *testing.T) {
	tests := []struct {
		name string
		argv []string
		want string
	}{
		{"empty argv", nil, ""},
		{"single program", []string{"ls"}, "ls"},
		{"plain args stay bare", []string{"go", "test", "./..."}, "go test ./..."},
		{"arg with space is quoted", []string{"echo", "a b"}, "echo 'a b'"},
		{"sh -c script keeps metacharacters intact", []string{"sh", "-c", "echo a; echo b"}, "sh -c 'echo a; echo b'"},
		{"empty arg is quoted", []string{"printf", ""}, "printf ''"},
		{"embedded single quote is escaped", []string{"echo", "it's"}, `echo 'it'"'"'s'`},
		{"safe punctuation stays bare", []string{"curl", "-H", "Accept:application/json"}, "curl -H Accept:application/json"},
		{"glob metacharacters are quoted", []string{"curl", "https://x.test/a,b?c=d"}, "curl 'https://x.test/a,b?c=d'"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := CommandLineForDisplay(tt.argv); got != tt.want {
				t.Fatalf("CommandLineForDisplay(%q) = %q, want %q", tt.argv, got, tt.want)
			}
		})
	}
}
