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

package domain

import "context"

// ToolContext identifies the durable evidence preceding a tool execution.
// The loop captures it after persisting execution-start events, before workers
// fan out, so tools never need to read the mutable run projection.
type ToolContext struct {
	SessionID     SessionID
	SourceVersion int64
}

type toolContextKey struct{}

func WithToolContext(ctx context.Context, source ToolContext) context.Context {
	return context.WithValue(ctx, toolContextKey{}, source)
}

// ToolContextFrom returns zero provenance for calls outside an agent loop.
func ToolContextFrom(ctx context.Context) ToolContext {
	source, _ := ctx.Value(toolContextKey{}).(ToolContext)
	return source
}
