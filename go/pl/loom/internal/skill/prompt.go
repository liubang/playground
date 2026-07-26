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
	"context"
)

// PromptProvider adapts Loader+Render to prompt.SkillsProvider (satisfied
// implicitly — this package does not import prompt). On every Build it
// re-loads the catalog, stores the fresh snapshot into the shared
// AtomicCatalog (so read_skill resolves against the same list the model
// sees), and renders the budgeted section body.
type PromptProvider struct {
	loader        *Loader
	catalog       *AtomicCatalog
	contextWindow int64
}

// NewPromptProvider wires the provider. catalog may be nil when no
// read_skill tool shares the snapshot.
func NewPromptProvider(loader *Loader, catalog *AtomicCatalog, contextWindow int64) *PromptProvider {
	return &PromptProvider{loader: loader, catalog: catalog, contextWindow: contextWindow}
}

// Skills reloads and renders the skills catalog. Loader never fails
// (individual skill failures degrade to LoadIssues), so this never returns
// an error; the signature matches prompt.SkillsProvider.
func (p *PromptProvider) Skills(ctx context.Context) (string, error) {
	if p.loader == nil {
		return "", nil
	}
	cat := p.loader.Load(ctx)
	if p.catalog != nil {
		p.catalog.Store(cat)
	}
	return Render(cat, p.contextWindow), nil
}
