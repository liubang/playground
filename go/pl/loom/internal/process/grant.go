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
// Created: 2026/07/27

package process

import "context"

// Grant describes per-call sandbox widenings decided by the policy layer
// (docs/PERMISSION_DESIGN.md §3.2). The zero value selects the runner's
// default sandbox unchanged; grants only ever widen it, never narrow it.
type Grant struct {
	// Unsandboxed runs outside the sandbox entirely (DirectSandbox).
	Unsandboxed bool
	// NetworkFull allows outbound network and DNS inside the sandbox.
	NetworkFull bool
	// WritablePaths are additional absolute paths writable inside the
	// sandbox. Protected workspace subpaths stay excluded regardless.
	WritablePaths []string
}

// IsZero reports whether the grant requests no widenings.
func (g Grant) IsZero() bool {
	return !g.Unsandboxed && !g.NetworkFull && len(g.WritablePaths) == 0
}

// RunWithGrant executes like RunWithSandbox, resolving the sandbox from a
// policy grant: unsandboxed grants escalate to DirectSandbox, widened
// grants clone the default sandbox with extra capabilities, and the zero
// grant uses the default sandbox unchanged.
func (r *Runner) RunWithGrant(ctx context.Context, spec CommandSpec, grant Grant) (Result, error) {
	return r.RunWithSandbox(ctx, spec, r.sandboxForGrant(grant))
}

// sandboxForGrant maps a policy grant onto a concrete sandbox. Widening an
// unsupported sandbox is a no-op (fail-closed is preserved); widening never
// turns a missing sandbox into a working one.
func (r *Runner) sandboxForGrant(grant Grant) Sandbox {
	switch {
	case grant.Unsandboxed:
		return DirectSandbox{}
	case grant.IsZero():
		return r.sandbox
	default:
		return widenSandbox(r.sandbox, grant.NetworkFull, grant.WritablePaths)
	}
}
