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
// Created: 2026/08/17

// Package kbsearch implements the knowledge base tools kb_search (hybrid
// retrieval) and kb_read (full document fetch), backed by an external
// minisearch server over its v2 REST API. Documents are uploaded to
// minisearch out of band; loom is a read-only consumer. The egress
// endpoint is pinned by the operator's knowledge_base configuration —
// never shaped by tool arguments — so unlike web_fetch the client needs
// no SSRF dial guard, and a private/loopback minisearch deployment is a
// legitimate target.
package kbsearch

import (
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// baseTool embeds the shared toolkit.BaseTool skeleton (definition +
// signer + prepare/verify protocol, REVIEW R3); kbsearch has no
// tool-specific dependencies beyond it.
type baseTool struct {
	toolkit.BaseTool
}

func newBaseTool(def domain.ToolDefinition) (baseTool, error) {
	bt, err := toolkit.NewBaseTool(def)
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{BaseTool: bt}, nil
}
