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
// Created: 2026/08/11

package permission

import (
	"regexp"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// userURLPattern finds http(s) URLs in free text. It is intentionally
// naive — trailing punctuation is stripped after the match — because the
// goal is recall of user-mentioned hosts, not RFC-compliant parsing;
// domain.HostFromURL is the real validator.
var userURLPattern = regexp.MustCompile(`https?://[^\s<>"'` + "`" + `]+`)

// urlTrailingPunct lists characters that commonly terminate a URL in
// prose but are never part of the host the user meant — including the
// CJK punctuation of Chinese prose ("看下 https://example.com。"), which
// would otherwise be parsed into the authority component and silently
// lose the mention.
const urlTrailingPunct = ".,;:!?)]}>'\"" + "。，、；：？！）】》」』“”‘’"

// ExtractUserIntentHosts scans user-role messages for http(s) URLs and
// returns the set of canonical hosts they mention (nil when none). Only
// user messages are consulted: assistant and tool content is
// model-influenced, and letting it seed the trust set would let the model
// self-authorize hosts.
func ExtractUserIntentHosts(messages []domain.Message) map[string]struct{} {
	var hosts map[string]struct{}
	for _, m := range messages {
		if m.Role != domain.RoleUser {
			continue
		}
		for _, p := range m.Parts {
			if p.Kind != domain.PartText {
				continue
			}
			for _, raw := range userURLPattern.FindAllString(p.Text, -1) {
				host, ok := domain.HostFromURL(strings.TrimRight(raw, urlTrailingPunct))
				if !ok {
					continue
				}
				if hosts == nil {
					hosts = make(map[string]struct{})
				}
				hosts[host] = struct{}{}
			}
		}
	}
	return hosts
}
