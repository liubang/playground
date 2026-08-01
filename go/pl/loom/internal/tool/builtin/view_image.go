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
	"encoding/base64"
	"encoding/json"
	"fmt"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"os"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	// maxImageBytes bounds the raw image payload. Base64 inflates by 4/3,
	// and Anthropic caps a single image block at 5MB on the wire, so 3.5MB
	// raw stays comfortably below the strictest provider limit.
	maxImageBytes = 3584 << 10
	maxImagePath  = 4096
)

type viewImageArgs struct {
	Path string `json:"path"`
}

// ViewImageTool implements view_image: attach a workspace image to the
// conversation so a vision-capable model can see it (codex's view_image).
type ViewImageTool struct {
	base baseTool
}

// NewViewImageTool creates a view_image tool bound to the workspace validator.
func NewViewImageTool(validator *workspacepkg.PathValidator) (*ViewImageTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "view_image",
		Description: "Attach a local image file (png, jpeg, gif, webp) from the workspace to the conversation so you " +
			"can see it. Use it whenever the user references an image (a screenshot, diagram, mockup, photo of an " +
			"error) that you need to look at. The result carries the image inline plus a text header with the " +
			"format, pixel dimensions when decodable, and byte size. Files larger than 3.5MB are rejected; " +
			"downscale or crop them first (e.g. with run_cmd sips/ImageMagick) if you must view them.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1,"maxLength":4096}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"string","description":"text header (path, media type, dimensions, size) followed by the image as an inline image content part"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ViewImageTool{base: base}, nil
}

func (t *ViewImageTool) Definition() domain.ToolDefinition {
	return t.base.def
}

// ConcurrentSafe implements domain.ConcurrentSafely: reads are independent.
func (t *ViewImageTool) ConcurrentSafe() bool { return true }

func (t *ViewImageTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[viewImageArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	pathInfo, err := resolveExistingPath(t.base.validator, args.Path)
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
	approvalDesc := fmt.Sprintf("View image %s", args.Path)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *ViewImageTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}
	args, err := decodeStrict[viewImageArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	pathInfo, err := resolveExistingPath(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != args.Path {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}
	if err := ctx.Err(); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	data, err := os.ReadFile(pathInfo.Absolute)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrUnavailable, "failed to read image file", domain.WithCause(err)))
	}
	if len(data) == 0 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "image file is empty"))
	}
	if len(data) > maxImageBytes {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("image is %d bytes, exceeding the %d byte limit; downscale or crop it first", len(data), maxImageBytes)))
	}

	mediaType := detectImageMediaType(data)
	if mediaType == "" {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput,
			"unsupported image format (want png, jpeg, gif, or webp magic bytes)"))
	}

	dimensions := imageDimensions(data, mediaType)
	header := fmt.Sprintf("image: %s · %s · %d bytes", args.Path, mediaType, len(data))
	if dimensions != "" {
		header += " · " + dimensions
	}

	finishedAt := time.Now()
	return domain.ToolResult{
		CallID: prepared.Call.ID,
		Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: header},
			{Kind: domain.PartImage, Image: &domain.ImageContent{
				MediaType: mediaType,
				Data:      base64.StdEncoding.EncodeToString(data),
			}},
		},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}

// detectImageMediaType sniffs the magic bytes of the four supported formats.
func detectImageMediaType(data []byte) string {
	switch {
	case len(data) >= 8 && string(data[:8]) == "\x89PNG\r\n\x1a\n":
		return "image/png"
	case len(data) >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF:
		return "image/jpeg"
	case len(data) >= 6 && (string(data[:6]) == "GIF87a" || string(data[:6]) == "GIF89a"):
		return "image/gif"
	case len(data) >= 12 && string(data[:4]) == "RIFF" && string(data[8:12]) == "WEBP":
		return "image/webp"
	}
	return ""
}

// imageDimensions reports "WxH" for formats the standard library can decode
// a config for (png, jpeg, gif); webp has no stdlib decoder and reports "".
func imageDimensions(data []byte, mediaType string) string {
	if mediaType == "image/webp" {
		return ""
	}
	cfg, _, err := image.DecodeConfig(strings.NewReader(string(data)))
	if err != nil {
		return ""
	}
	return fmt.Sprintf("%dx%d", cfg.Width, cfg.Height)
}
