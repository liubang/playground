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
	"testing"
)

func dirSet(paths ...string) func(string) bool {
	set := make(map[string]bool, len(paths))
	for _, p := range paths {
		set[p] = true
	}
	return func(path string) bool { return set[path] }
}

func TestAugmentedPATHAppendsExistingToolchainDirs(t *testing.T) {
	got := AugmentedPATH("/usr/bin:/bin", "/home/u",
		dirSet("/opt/homebrew/bin", "/home/u/.local/share/mise/shims"))
	want := "/usr/bin:/bin:/opt/homebrew/bin:/home/u/.local/share/mise/shims"
	if got != want {
		t.Fatalf("AugmentedPATH = %q, want %q", got, want)
	}
}

func TestAugmentedPATHSkipsMissingDirs(t *testing.T) {
	got := AugmentedPATH("/usr/bin", "/home/u", dirSet())
	if got != "/usr/bin" {
		t.Fatalf("AugmentedPATH = %q, want unchanged base", got)
	}
}

func TestAugmentedPATHKeepsExistingEntriesInPlace(t *testing.T) {
	// A dir already on PATH keeps its original (earlier) position: the
	// user's own precedence wins, the append is skipped.
	got := AugmentedPATH("/home/u/.cargo/bin:/usr/bin", "/home/u",
		dirSet("/home/u/.cargo/bin", "/opt/homebrew/bin"))
	want := "/home/u/.cargo/bin:/usr/bin:/opt/homebrew/bin"
	if got != want {
		t.Fatalf("AugmentedPATH = %q, want %q", got, want)
	}
}

func TestAugmentedPATHEmptyBaseStartsFromDefault(t *testing.T) {
	got := AugmentedPATH("", "/home/u", dirSet("/usr/local/bin"))
	want := defaultPATH + ":/usr/local/bin"
	if got != want {
		t.Fatalf("AugmentedPATH(\"\") = %q, want %q", got, want)
	}
}

func TestAugmentedPATHSkipsTildeEntriesWithoutHome(t *testing.T) {
	got := AugmentedPATH("/usr/bin", "", dirSet("/opt/homebrew/bin"))
	want := "/usr/bin:/opt/homebrew/bin"
	if got != want {
		t.Fatalf("AugmentedPATH = %q, want %q", got, want)
	}
}

func TestAugmentedPATHIsIdempotent(t *testing.T) {
	exists := dirSet("/opt/homebrew/bin", "/home/u/.cargo/bin")
	once := AugmentedPATH("/usr/bin", "/home/u", exists)
	twice := AugmentedPATH(once, "/home/u", exists)
	if once != twice {
		t.Fatalf("not idempotent: once=%q twice=%q", once, twice)
	}
	if strings.Count(twice, "/opt/homebrew/bin") != 1 {
		t.Fatalf("duplicate appended: %q", twice)
	}
}
