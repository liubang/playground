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
// Created: 2026/08/01

package builtin

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/media"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// imagePathArgs is the shared argument shape of the two image path tools
// (view_image, present_image).
type imagePathArgs struct {
	Path string `json:"path"`
}

// prepareImagePathCall implements the shared Prepare pipeline of the image
// path tools: decode args, resolve the path inside the workspace validator,
// and bind the canonical path into the prepared call.
func prepareImagePathCall(base baseTool, ctx context.Context, call domain.ToolCall, approvalVerb string) (domain.PreparedCall, error) {
	args, err := decodeStrict[imagePathArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	pathInfo, err := resolveExistingPath(base.validator, args.Path)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if pathInfo.Info.IsDir() {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a file")
	}
	args.Path = pathInfo.Display

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	return base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{
		ReadPaths:    []string{pathInfo.Absolute},
		ApprovalDesc: fmt.Sprintf("%s %s", approvalVerb, args.Path),
	})
}

// loadImageArtifact implements the shared Execute pipeline of the image
// path tools: re-verify the prepared binding, read the file (size-checked
// before the read, REVIEW M25), and persist the bytes as an artifact. The
// media type is sniffed from the bytes — never from the file extension.
func loadImageArtifact(base baseTool, artifacts domain.ArtifactStore, ctx context.Context, prepared domain.PreparedCall) (string, domain.ArtifactRef, []byte, error) {
	if err := base.VerifyPreparedCall(prepared); err != nil {
		return "", domain.ArtifactRef{}, nil, err
	}
	if len(prepared.ReadPaths) != 1 {
		return "", domain.ArtifactRef{}, nil, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid")
	}
	args, err := decodeStrict[imagePathArgs](prepared.Call.Arguments)
	if err != nil {
		return "", domain.ArtifactRef{}, nil, err
	}
	pathInfo, err := resolveExistingPath(base.validator, prepared.ReadPaths[0])
	if err != nil {
		return "", domain.ArtifactRef{}, nil, err
	}
	if pathInfo.Display != args.Path {
		return "", domain.ArtifactRef{}, nil, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch")
	}
	if err := ctx.Err(); err != nil {
		return "", domain.ArtifactRef{}, nil, err
	}
	if pathInfo.Info != nil && pathInfo.Info.Size() > media.MaxImageBytes {
		return "", domain.ArtifactRef{}, nil, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("image is %d bytes, exceeding the %d byte limit; downscale or crop it first", pathInfo.Info.Size(), media.MaxImageBytes))
	}
	data, err := os.ReadFile(pathInfo.Absolute)
	if err != nil {
		return "", domain.ArtifactRef{}, nil, domain.NewError(domain.ErrUnavailable, "failed to read image file", domain.WithCause(err))
	}
	if len(data) == 0 {
		return "", domain.ArtifactRef{}, nil, domain.NewError(domain.ErrInvalidInput, "image file is empty")
	}
	ref, err := media.StoreImage(ctx, artifacts, data)
	if err != nil {
		return "", domain.ArtifactRef{}, nil, err
	}
	return args.Path, ref, data, nil
}

// imageHeader builds the shared model-facing header line for the image
// path tools (path · media type · byte size · pixel dimensions).
func imageHeader(displayPath string, ref domain.ArtifactRef, data []byte) string {
	header := fmt.Sprintf("image: %s · %s · %d bytes", displayPath, ref.MediaType, len(data))
	if dimensions := media.Dimensions(data); dimensions != "" {
		header += " · " + dimensions
	}
	return header
}

// ViewImageTool implements view_image: attach a workspace image to the
// conversation so a vision-capable model can see it (codex's view_image).
// The raw bytes are persisted as an artifact, and the agent loop
// materializes a derived (rescaled) inline image for the model at the
// egress (media.Materialize). Nothing is ever inlined into the transcript.
// The artifact part is marked ModelOnly: display channels show only the
// text header, never the image itself — displaying an image to the user
// is present_image's job.
type ViewImageTool struct {
	base      baseTool
	artifacts domain.ArtifactStore
}

// NewViewImageTool creates a view_image tool bound to the workspace validator.
// The artifact store is required: it is the image's only delivery channel.
func NewViewImageTool(validator *workspacepkg.PathValidator, artifacts domain.ArtifactStore) (*ViewImageTool, error) {
	if artifacts == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "view_image artifact store is required")
	}
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "view_image",
		Description: "Attach a local image file (png, jpeg, gif, webp) to the conversation so you " +
			"can see it. Relative paths resolve inside the workspace; absolute paths outside the workspace work " +
			"too (credential locations are always denied). Use it only for image files on disk that are NOT " +
			"already in the conversation — e.g. when the user gives you a path to a screenshot, diagram, " +
			"mockup, or photo of an error that you need to look at. Images the user attached directly to " +
			"their message are already provided to you inline: never call this tool for those, and never " +
			"guess or reconstruct a file path from memory or context — call it only with an explicit path " +
			"given by the user or one you have verified exists. The image is persisted " +
			"as an artifact and attached for your review at model-request time (large images are rescaled " +
			"automatically); it is NOT shown to the user — call present_image as well if the user should " +
			"see it. Files larger than 64MB are rejected; downscale or crop them first (e.g. with run_cmd " +
			"sips/ImageMagick) if you must view them. If you only want to DISPLAY an image to the user " +
			"(e.g. a plot you just generated) without seeing it yourself, use present_image instead.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1,"maxLength":4096}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"string","description":"text header (path, media type, dimensions, size) followed by an artifact reference to the image"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ViewImageTool{base: base, artifacts: artifacts}, nil
}

func (t *ViewImageTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: reads are independent.
func (t *ViewImageTool) ConcurrentSafe() bool { return true }

func (t *ViewImageTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	return prepareImagePathCall(t.base, ctx, call, "View image")
}

func (t *ViewImageTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	path, ref, data, err := loadImageArtifact(t.base, t.artifacts, ctx, prepared)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	header := imageHeader(path, ref, data) +
		"\nNote: the image is attached for your review at model-request time. It is not displayed " +
		"to the user — call present_image if they should see it, and do not embed it as a markdown " +
		"link in your reply."
	return domain.ToolResult{
		CallID: prepared.Call.ID,
		Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: header},
			{Kind: domain.PartArtifact, Artifact: &ref, ModelOnly: true},
		},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}
