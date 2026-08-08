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

import (
	"regexp"
	"strings"
	"testing"
)

var semverPattern = regexp.MustCompile(`^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$`)

func TestVersionShape(t *testing.T) {
	if !semverPattern.MatchString(Version) {
		t.Fatalf("Version = %q, want semver like 1.2.3 or 1.2.3-dev", Version)
	}
	if !strings.HasPrefix(Version, Release) {
		t.Fatalf("Release = %q is not a prefix of Version = %q", Release, Version)
	}
	if strings.Contains(Release, "-") {
		t.Fatalf("Release = %q must not carry a pre-release suffix", Release)
	}
}
