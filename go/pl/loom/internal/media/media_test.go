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

package media

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"hash/crc32"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// --- fixtures ---

func solidImage(w, h int, opaque bool) image.Image {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	alpha := uint8(255)
	if !opaque {
		alpha = 128
	}
	c := color.NRGBA{R: 200, G: 100, B: 50, A: alpha}
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.SetNRGBA(x, y, c)
		}
	}
	return img
}

func pngBytes(t *testing.T, w, h int, opaque bool) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := png.Encode(&buf, solidImage(w, h, opaque)); err != nil {
		t.Fatalf("encode png: %v", err)
	}
	return buf.Bytes()
}

func jpegBytes(t *testing.T, w, h int) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, solidImage(w, h, true), nil); err != nil {
		t.Fatalf("encode jpeg: %v", err)
	}
	return buf.Bytes()
}

// fakePNGHeader builds a PNG whose IHDR claims the given dimensions (with a
// valid CRC) but carries no real pixel data — enough to exercise
// DecodeConfig-level guards without allocating gigabytes.
func fakePNGHeader(w, h int) []byte {
	var buf bytes.Buffer
	buf.WriteString("\x89PNG\r\n\x1a\n")
	ihdr := make([]byte, 13)
	binary.BigEndian.PutUint32(ihdr[0:4], uint32(w))
	binary.BigEndian.PutUint32(ihdr[4:8], uint32(h))
	ihdr[8] = 8 // bit depth
	ihdr[9] = 6 // color type RGBA
	_ = binary.Write(&buf, binary.BigEndian, uint32(len(ihdr)))
	buf.WriteString("IHDR")
	buf.Write(ihdr)
	crc := crc32.NewIEEE()
	crc.Write([]byte("IHDR"))
	crc.Write(ihdr)
	_ = binary.Write(&buf, binary.BigEndian, crc.Sum32())
	return buf.Bytes()
}

func openStore(t *testing.T) *artifact.Store {
	t.Helper()
	store, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), 64<<20)
	if err != nil {
		t.Fatalf("open artifact store: %v", err)
	}
	return store
}

// --- SniffImageType ---

func TestSniffImageType(t *testing.T) {
	cases := []struct {
		name string
		data []byte
		want string
	}{
		{"png", pngBytes(t, 4, 4, true), "image/png"},
		{"jpeg", jpegBytes(t, 4, 4), "image/jpeg"},
		{"gif", []byte("GIF89a...."), "image/gif"},
		{"webp", []byte("RIFFxxxxWEBP"), "image/webp"},
		{"text", []byte("hello world, not an image"), ""},
		{"empty", nil, ""},
	}
	for _, tt := range cases {
		if got := SniffImageType(tt.data); got != tt.want {
			t.Errorf("%s: SniffImageType() = %q, want %q", tt.name, got, tt.want)
		}
	}
}

// --- Derive ---

func TestDeriveSmallOpaqueJPEGBecomesJPEG(t *testing.T) {
	img, err := Derive(jpegBytes(t, 100, 50))
	if err != nil {
		t.Fatalf("Derive: %v", err)
	}
	if img.MediaType != "image/jpeg" {
		t.Fatalf("MediaType = %q, want image/jpeg (opaque re-encode)", img.MediaType)
	}
	assertDimensions(t, img, 100, 50)
}

func TestDeriveKeepsAlphaAsPNG(t *testing.T) {
	img, err := Derive(pngBytes(t, 64, 64, false))
	if err != nil {
		t.Fatalf("Derive: %v", err)
	}
	if img.MediaType != "image/png" {
		t.Fatalf("MediaType = %q, want image/png (alpha preserved)", img.MediaType)
	}
	assertDimensions(t, img, 64, 64)
}

func TestDeriveDownscalesToPixelBound(t *testing.T) {
	img, err := Derive(pngBytes(t, 4096, 1024, true))
	if err != nil {
		t.Fatalf("Derive: %v", err)
	}
	assertDimensions(t, img, maxDimension, 512)
}

func TestDeriveRejectsNonImage(t *testing.T) {
	if _, err := Derive([]byte("definitely not an image")); err == nil {
		t.Fatal("Derive(non-image) should fail")
	}
}

func TestDeriveRejectsPixelBomb(t *testing.T) {
	if _, err := Derive(fakePNGHeader(10000, 10000)); err == nil {
		t.Fatal("Derive(pixel bomb) should fail at the DecodeConfig guard")
	}
}

func assertDimensions(t *testing.T, img domain.ImageContent, wantW, wantH int) {
	t.Helper()
	raw, err := base64.StdEncoding.DecodeString(img.Data)
	if err != nil {
		t.Fatalf("base64 decode: %v", err)
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("decode derived image: %v", err)
	}
	if cfg.Width != wantW || cfg.Height != wantH {
		t.Fatalf("dimensions = %dx%d, want %dx%d", cfg.Width, cfg.Height, wantW, wantH)
	}
}

// --- StoreImage ---

func TestStoreImage(t *testing.T) {
	store := openStore(t)
	ref, err := StoreImage(context.Background(), store, pngBytes(t, 8, 8, true))
	if err != nil {
		t.Fatalf("StoreImage: %v", err)
	}
	if ref.MediaType != "image/png" {
		t.Fatalf("ref.MediaType = %q, want image/png (sniffed)", ref.MediaType)
	}
	if _, err := store.Read(context.Background(), ref); err != nil {
		t.Fatalf("stored artifact unreadable: %v", err)
	}
}

func TestStoreImageRejectsNonImage(t *testing.T) {
	if _, err := StoreImage(context.Background(), openStore(t), []byte("nope")); err == nil {
		t.Fatal("StoreImage(non-image) should fail before touching the store")
	}
}

// --- Materialize ---

func userMessageWithRef(t *testing.T, ref domain.ArtifactRef) domain.Message {
	t.Helper()
	return domain.Message{
		ID:   domain.NewMessageID(),
		Role: domain.RoleUser,
		Parts: []domain.ContentPart{
			{Kind: domain.PartText, Text: "look at this"},
			{Kind: domain.PartArtifact, Artifact: &ref},
		},
	}
}

func TestMaterializeNilStorePassthrough(t *testing.T) {
	msgs := []domain.Message{userMessageWithRef(t, domain.ArtifactRef{})}
	if got := Materialize(context.Background(), nil, msgs); &got[0] != &msgs[0] {
		t.Fatal("nil store must return the input slice untouched")
	}
}

func TestMaterializeNoImagesPassthrough(t *testing.T) {
	msgs := []domain.Message{{
		ID:    domain.NewMessageID(),
		Role:  domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "plain"}},
	}}
	if got := Materialize(context.Background(), openStore(t), msgs); &got[0] != &msgs[0] {
		t.Fatal("messages without image refs must pass through zero-copy")
	}
}

func TestMaterializeResolvesImageRef(t *testing.T) {
	store := openStore(t)
	raw := pngBytes(t, 3000, 3000, true) // exceeds the pixel bound
	ref, err := StoreImage(context.Background(), store, raw)
	if err != nil {
		t.Fatalf("StoreImage: %v", err)
	}
	canonical := []domain.Message{userMessageWithRef(t, ref)}

	out := Materialize(context.Background(), store, canonical)

	parts := out[0].Parts
	// Layout: original text, attachment note, materialized image.
	if len(parts) != 3 {
		t.Fatalf("parts = %d, want 3 (text, note, image)", len(parts))
	}
	if parts[1].Kind != domain.PartText || !strings.Contains(parts[1].Text, "view_image") {
		t.Fatalf("part[1] = %+v, want the attachment note", parts[1])
	}
	if parts[2].Kind != domain.PartImage || parts[2].Image == nil {
		t.Fatalf("part[2] = %+v, want a materialized image part", parts[2])
	}
	// The stored original is 3000px; the derived wire image must be bounded.
	assertDimensions(t, *parts[2].Image, maxDimension, maxDimension)
	// Canonical history must not be mutated.
	if canonical[0].Parts[1].Kind != domain.PartArtifact {
		t.Fatal("Materialize mutated the canonical message")
	}
}

func TestMaterializeResolvesToolResultImage(t *testing.T) {
	store := openStore(t)
	ref, err := StoreImage(context.Background(), store, pngBytes(t, 10, 10, true))
	if err != nil {
		t.Fatalf("StoreImage: %v", err)
	}
	msgs := []domain.Message{{
		ID:   domain.NewMessageID(),
		Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
			CallID: domain.NewToolCallID(),
			Status: domain.ToolStatusSuccess,
			Content: []domain.ContentPart{
				{Kind: domain.PartText, Text: "image: x.png"},
				{Kind: domain.PartArtifact, Artifact: &ref},
			},
		}}},
	}}
	out := Materialize(context.Background(), store, msgs)
	content := out[0].Parts[0].ToolResult.Content
	if content[1].Kind != domain.PartImage {
		t.Fatalf("tool-result content[1] = %v, want image", content[1].Kind)
	}
	if msgs[0].Parts[0].ToolResult.Content[1].Kind != domain.PartArtifact {
		t.Fatal("Materialize mutated the canonical tool result")
	}
}

func TestMaterializeMissingArtifactDegradesToText(t *testing.T) {
	store := openStore(t)
	id, err := domain.ParseArtifactID("art_sha256_" + strings.Repeat("0", 64))
	if err != nil {
		t.Fatalf("parse artifact id: %v", err)
	}
	msgs := []domain.Message{userMessageWithRef(t, domain.ArtifactRef{ID: id, Size: 10, MediaType: "image/png"})}
	out := Materialize(context.Background(), store, msgs)
	part := out[0].Parts[2] // after the original text and the attachment note
	if part.Kind != domain.PartText || !strings.Contains(part.Text, "unavailable") {
		t.Fatalf("part[2] = %+v, want a text placeholder", part)
	}
}

// Display flags: ModelOnly governs display only and stays model-bound;
// PresentOnly is display-only and must pass through unresolved.
func TestMaterializeRespectsDisplayFlags(t *testing.T) {
	store := openStore(t)
	ref, err := StoreImage(context.Background(), store, pngBytes(t, 10, 10, true))
	if err != nil {
		t.Fatalf("StoreImage: %v", err)
	}
	msgs := []domain.Message{{
		ID:   domain.NewMessageID(),
		Role: domain.RoleUser,
		Parts: []domain.ContentPart{
			{Kind: domain.PartArtifact, Artifact: &ref, ModelOnly: true},
			{Kind: domain.PartArtifact, Artifact: &ref, PresentOnly: true},
		},
	}}
	out := Materialize(context.Background(), store, msgs)
	parts := out[0].Parts
	// The model-only ref expands to note+image; the present-only ref is untouched.
	if parts[0].Kind != domain.PartText {
		t.Fatalf("part[0] = %v, want the attachment note", parts[0].Kind)
	}
	if parts[1].Kind != domain.PartImage {
		t.Fatalf("model-only part = %v, want a materialized image part", parts[1].Kind)
	}
	if parts[2].Kind != domain.PartArtifact || !parts[2].PresentOnly {
		t.Fatalf("present-only part = %+v, want the artifact reference untouched", parts[2])
	}
}

func TestMaterializeLeavesNonImageArtifacts(t *testing.T) {
	store := openStore(t)
	ref, err := store.PutBytes(context.Background(), []byte("plain text log"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	ref.MediaType = "text/plain"
	msgs := []domain.Message{userMessageWithRef(t, ref)}
	if got := Materialize(context.Background(), store, msgs); &got[0] != &msgs[0] {
		t.Fatal("non-image artifact refs must not be materialized")
	}
}

// --- cache ---

func TestImageCacheLRU(t *testing.T) {
	c := newImageCache(10)
	img := func(b byte) domain.ImageContent {
		return domain.ImageContent{MediaType: "image/png", Data: string(make([]byte, 4)) + string(b)}
	}
	c.put("a", img('a'))
	c.put("b", img('b')) // total 10, full
	if _, ok := c.get("a"); !ok {
		t.Fatal("a should be cached")
	}
	c.put("c", img('c')) // evicts b (LRU), not a (recently used)
	if _, ok := c.get("b"); ok {
		t.Fatal("b should have been evicted")
	}
	if _, ok := c.get("a"); !ok {
		t.Fatal("a should survive (most recently used)")
	}
}

func TestImageCacheSkipsOversized(t *testing.T) {
	c := newImageCache(4)
	c.put("big", domain.ImageContent{Data: strings.Repeat("x", 8)})
	if _, ok := c.get("big"); ok {
		t.Fatal("entries larger than the cache must not be stored")
	}
}
