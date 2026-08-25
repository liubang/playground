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
	"testing"
)

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
