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
// Created: 2026/08/12

package builtin

import (
	"context"
	"encoding/json"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// PresentImageTool implements present_image: display a local image to the
// USER without attaching it to the model's context. It is the counterpart
// of view_image (which attaches the image for the model to see): the image
// is persisted as an artifact and rendered in the user's transcript, but
// the part is marked present-only, so the egress materialization skips it
// and it costs no context and no tokens.
type PresentImageTool struct {
	base      baseTool
	artifacts domain.ArtifactStore
}

// NewPresentImageTool creates a present_image tool bound to the workspace
// validator. The artifact store is required: it is the image's only
// delivery channel.
func NewPresentImageTool(validator *workspacepkg.PathValidator, artifacts domain.ArtifactStore) (*PresentImageTool, error) {
	if artifacts == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "present_image artifact store is required")
	}
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "present_image",
		Description: "Display a local image file (png, jpeg, gif, webp) to the user, e.g. a plot, diagram, " +
			"or render you just produced. The image is shown in the user's transcript; you do NOT see it " +
			"yourself and it does not enter your context. Use this whenever the goal is to show an image " +
			"rather than to inspect one; use view_image when you need to look at an image. Relative paths " +
			"resolve inside the workspace; absolute paths outside the workspace work too (credential " +
			"locations are always denied).",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1,"maxLength":4096}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"string","description":"text header (path, media type, dimensions, size) confirming the image was displayed"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &PresentImageTool{base: base, artifacts: artifacts}, nil
}

func (t *PresentImageTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: reads are independent.
func (t *PresentImageTool) ConcurrentSafe() bool { return true }

func (t *PresentImageTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	return prepareImagePathCall(t.base, ctx, call, "Present image")
}

func (t *PresentImageTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	path, ref, data, err := loadImageArtifact(t.base, t.artifacts, ctx, prepared)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	header := imageHeader(path, ref, data) +
		"\nNote: the image is now displayed to the user. It is not attached to your context; " +
		"call view_image if you later need to inspect it."
	return domain.ToolResult{
		CallID: prepared.Call.ID,
		Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: header},
			{Kind: domain.PartArtifact, Artifact: &ref, PresentOnly: true},
		},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}
