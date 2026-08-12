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

// Package media is the single home for loom's image pipeline. Images follow
// a store-original / derive-at-egress model (codex-style):
//
//   - Ingress (tools, user attachments) stores the ORIGINAL bytes in the
//     artifact store and records an ArtifactRef in the transcript; no image
//     bytes are ever inlined into persisted messages.
//   - Egress (Materialize, called by the agent loop before every model
//     request) resolves image artifact references into freshly derived,
//     provider-safe inline images: decoded, downscaled to the profile's
//     pixel bound, and re-encoded. Provider size limits and token cost are
//     absorbed here instead of leaking into user-facing ingress limits.
//
// Derived images are memoized in a process-level cache keyed by the
// artifact's content hash, so repeated model calls over the same history
// do not re-decode or re-resize.
package media

import (
	"bytes"
	"fmt"
	"image"
	// Register the decoders Derive relies on: png/jpeg/gif from the stdlib,
	// webp from x/image (decode-only; derived output is always png or jpeg).
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"

	_ "golang.org/x/image/webp"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	// MaxImageBytes bounds the raw bytes one image ingress accepts
	// (view_image, user attachments). The artifact store's own limit is the
	// hard backstop; this ingress bound exists so oversized files are
	// rejected BEFORE being read fully into memory. Provider wire limits do
	// not leak here: the egress derivation rescales to maxDimension.
	MaxImageBytes = 64 << 20
	// maxDimension bounds the width and height of a derived image. 2048px
	// keeps every mainstream vision provider's pricing/optimum sweet spot
	// (Anthropic ~1568px, OpenAI 512px tiles) while bounding the token cost
	// of one image regardless of the source resolution.
	maxDimension = 2048
	// maxPixels bounds the DECODED pixel count as a decompression-bomb
	// guard: a hostile or corrupt file must not exhaust memory before the
	// pixel bound above can apply. 32M pixels ≈ 128MB as RGBA.
	maxPixels = 32_000_000
	// jpegQuality is the re-encode quality for opaque derived images.
	jpegQuality = 85
)

// SniffImageType reports the image media type declared by the magic bytes
// of data, or "" when data is not a supported image (png, jpeg, gif, webp).
func SniffImageType(data []byte) string {
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

// IsImageMediaType reports whether mediaType is a supported image type.
// It is the media-type-level counterpart of domain.IsImageExtension and is
// used to decide which artifact references Materialize resolves.
func IsImageMediaType(mediaType string) bool {
	switch mediaType {
	case "image/png", "image/jpeg", "image/gif", "image/webp":
		return true
	}
	return false
}

// Dimensions reports "WxH" for a decodable image, "" otherwise. With the
// webp decoder registered above, DecodeConfig covers every supported format.
func Dimensions(raw []byte) string {
	cfg, _, err := image.DecodeConfig(bytes.NewReader(raw))
	if err != nil {
		return ""
	}
	return fmt.Sprintf("%dx%d", cfg.Width, cfg.Height)
}

// Derive decodes raw image bytes, downscales to the pixel bound, and
// re-encodes as png (alpha) or jpeg (opaque), returning the base64 inline
// form consumed by model providers. The input media type is SNIFFED, never
// trusted from the caller.
func Derive(raw []byte) (domain.ImageContent, error) {
	// DecodeConfig first: cheaply rejects non-images and oversized pixel
	// counts without materializing the full bitmap.
	cfg, _, err := image.DecodeConfig(bytes.NewReader(raw))
	if err != nil {
		return domain.ImageContent{}, domain.NewError(domain.ErrInvalidInput,
			"media: not a decodable image (want png, jpeg, gif, or webp)", domain.WithCause(err))
	}
	if cfg.Width <= 0 || cfg.Height <= 0 || int64(cfg.Width)*int64(cfg.Height) > maxPixels {
		return domain.ImageContent{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("media: image is %dx%d, exceeding the %d pixel limit", cfg.Width, cfg.Height, maxPixels))
	}

	img, _, err := image.Decode(bytes.NewReader(raw))
	if err != nil {
		return domain.ImageContent{}, domain.NewError(domain.ErrInvalidInput,
			"media: decode image", domain.WithCause(err))
	}
	img = downscale(img, maxDimension)
	return encode(img)
}
