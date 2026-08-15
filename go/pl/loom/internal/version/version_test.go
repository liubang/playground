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

package version

import "testing"

// TestUnstampedDefaults: `go test` binaries are never stamped; the
// package must boot with the documented default instead of panicking
// or guessing a number.
func TestUnstampedDefaults(t *testing.T) {
	if Version != "dev" {
		t.Fatalf("unstamped Version = %q, want %q", Version, "dev")
	}
	if Release != "dev" {
		t.Fatalf("unstamped Release = %q, want %q", Release, "dev")
	}
}

// TestReleaseOf: a stamped version is "<yyyymmdd>.<git-short-hash>";
// Release keeps only the date segment (CFBundleShortVersionString is
// numeric-only).
func TestReleaseOf(t *testing.T) {
	cases := map[string]string{
		"20260815.82f4e2a53": "20260815",
		"20260815.unknown":   "20260815",
		"dev":                "dev",
	}
	for in, want := range cases {
		if got := releaseOf(in); got != want {
			t.Errorf("releaseOf(%q) = %q, want %q", in, got, want)
		}
	}
}
