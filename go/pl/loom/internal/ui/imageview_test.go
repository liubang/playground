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

package ui

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"strings"
	"testing"

	"github.com/charmbracelet/lipgloss"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/ui/termimage"
)

func fakePNG(t *testing.T, w, h int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.Set(x, y, color.RGBA{uint8(x * 3), uint8(y * 5), 200, 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("png: %v", err)
	}
	return buf.Bytes()
}

func artifactID(t *testing.T, s string) domain.ArtifactID {
	t.Helper()
	id, err := domain.ParseArtifactID(s)
	if err != nil {
		t.Fatalf("artifact id %q: %v", s, err)
	}
	return id
}

func toolCallID(t *testing.T, s string) domain.ToolCallID {
	t.Helper()
	id, err := domain.ParseToolCallID(s)
	if err != nil {
		t.Fatalf("tool call id %q: %v", s, err)
	}
	return id
}

// TestPlaceholderLineMeasuredWidth pins the layout-critical property: a
// placeholder cell (U+10EEEE + two combining diacritics) must measure as
// exactly one display column, or image rows would misalign the transcript.
func TestPlaceholderLineMeasuredWidth(t *testing.T) {
	var out bytes.Buffer
	k := termimage.NewKitty(&out)
	lines, err := k.Render("w", fakePNG(t, 40, 10), 16)
	if err != nil {
		t.Fatalf("Render: %v", err)
	}
	for i, line := range lines {
		// lipgloss strips SGR before measuring; the placeholder runes are
		// what the transcript layout actually accounts for.
		if got := lipgloss.Width(line); got != 16 {
			t.Fatalf("line %d display width = %d, want 16", i, got)
		}
	}
}

// TestRebuildTranscriptCreatesImageBlocks verifies the snapshot path: a
// tool result carrying a present-only image artifact yields an image block
// placed right after its tool call.
func TestRebuildTranscriptCreatesImageBlocks(t *testing.T) {
	ref := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("ab", 32)), Size: 5}
	callID := toolCallID(t, "call-1")
	messages := []domain.Message{
		{
			ID:   domain.NewMessageID(),
			Role: domain.RoleAssistant,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID, Name: "present_image"}},
			},
		},
		{
			ID:   domain.NewMessageID(),
			Role: domain.RoleAssistant,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
					CallID: callID,
					Status: domain.ToolStatusSuccess,
					Content: []domain.ContentPart{
						{Kind: domain.PartText, Text: "ok"},
						{Kind: domain.PartArtifact, Artifact: &ref, PresentOnly: true},
					},
				}},
			},
		},
	}
	idx := RebuildTranscript(messages)
	var toolIdx, imgIdx int
	for i, id := range idx.Order {
		switch idx.ByID[id].Kind {
		case BlockKindTool:
			toolIdx = i
		case BlockKindImage:
			imgIdx = i
		}
	}
	if imgIdx == 0 || imgIdx != toolIdx+1 {
		t.Fatalf("image block not directly after tool block: tool=%d img=%d order=%v", toolIdx, imgIdx, idx.Order)
	}
	if b := idx.ByID["img-"+ref.ID.String()]; b == nil || b.ImageRef.ID != ref.ID {
		t.Fatalf("image block missing ref: %+v", b)
	}
}

// TestLockedTermWriterSatisfiesTermFile pins the bubbletea TTY contract:
// program startup does p.output.(term.File) (io.ReadWriteCloser + Fd) to
// enable window sizing and resize events. If the wrapper ever stops
// satisfying it, the TUI silently renders nothing.
func TestLockedTermWriterSatisfiesTermFile(t *testing.T) {
	type termFile interface {
		io.ReadWriteCloser
		Fd() uintptr
	}
	if _, ok := any(newLockedTermWriter(os.Stdout)).(termFile); !ok {
		t.Fatal("lockedTermWriter must satisfy term.File for bubbletea TTY detection")
	}
}

// TestRebuildTranscriptKeepsMultiImageOrder pins the chained-anchor
// behavior: several images under one tool result keep their original order.
func TestRebuildTranscriptKeepsMultiImageOrder(t *testing.T) {
	ref1 := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("ab", 32)), Size: 5}
	ref2 := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("cd", 32)), Size: 5}
	callID := toolCallID(t, "call-2")
	messages := []domain.Message{
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Parts: []domain.ContentPart{
			{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID, Name: "generate_image"}},
		}},
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Parts: []domain.ContentPart{
			{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
				CallID: callID, Status: domain.ToolStatusSuccess,
				Content: []domain.ContentPart{
					{Kind: domain.PartArtifact, Artifact: &ref1, PresentOnly: true},
					{Kind: domain.PartArtifact, Artifact: &ref2, PresentOnly: true},
				},
			}},
		}},
	}
	idx := RebuildTranscript(messages)
	var imgs []string
	for _, id := range idx.Order {
		if idx.ByID[id].Kind == BlockKindImage {
			imgs = append(imgs, id)
		}
	}
	want := []string{"img-" + ref1.ID.String(), "img-" + ref2.ID.String()}
	if len(imgs) != 2 || imgs[0] != want[0] || imgs[1] != want[1] {
		t.Fatalf("image order = %v, want %v (order=%v)", imgs, want, idx.Order)
	}
}

// TestRebuildTranscriptSkipsModelOnlyArtifacts pins the display contract:
// view_image artifacts (model-bound) never become image blocks.
func TestRebuildTranscriptSkipsModelOnlyArtifacts(t *testing.T) {
	ref := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("cd", 32)), Size: 5}
	callID := toolCallID(t, "call-1")
	messages := []domain.Message{
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Parts: []domain.ContentPart{
			{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID, Name: "view_image"}},
		}},
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Parts: []domain.ContentPart{
			{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
				CallID: callID, Status: domain.ToolStatusSuccess,
				Content: []domain.ContentPart{
					{Kind: domain.PartArtifact, Artifact: &ref, ModelOnly: true},
				},
			}},
		}},
	}
	idx := RebuildTranscript(messages)
	for _, id := range idx.Order {
		if idx.ByID[id].Kind == BlockKindImage {
			t.Fatalf("model-only artifact rendered as image block %s", id)
		}
	}
}

// TestHandleImageBytesMsgRendersAndFails exercises the async completion
// paths of an image block.
func TestHandleImageBytesMsgRendersAndFails(t *testing.T) {
	ref := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("ef", 32)), Size: 5}
	idx := NewBlockIndex()
	img := &TranscriptBlock{ID: "img-x", Kind: BlockKindImage, ImageRef: ref, Done: true}
	idx.Add(img)
	m := Model{blocks: idx, width: 120, theme: DetectTheme()}

	// Without an engine the block reports an unsupported-terminal error.
	m.handleImageBytesMsg(imageBytesMsg{blockID: "img-x", data: fakePNG(t, 20, 20), err: nil})
	if img.ImageErr == "" || len(img.ImageLines) != 0 {
		t.Fatalf("expected unsupported-terminal error, got lines=%d err=%q", len(img.ImageLines), img.ImageErr)
	}

	// With an engine the block renders placeholder lines.
	var out bytes.Buffer
	m.images = termimage.NewKitty(&out)
	rendered := m.handleImageBytesMsg(imageBytesMsg{blockID: "img-x", data: fakePNG(t, 20, 20), err: nil})
	if img.ImageErr != "" || len(img.ImageLines) == 0 {
		t.Fatalf("render failed: err=%q lines=%d", img.ImageErr, len(img.ImageLines))
	}
	if !strings.Contains(out.String(), "\x1b_G") {
		t.Fatal("kitty transmission escape missing")
	}
	if !strings.Contains(rendered.renderImage(img), "\U0010EEEE") {
		t.Fatal("placeholder cells missing from rendered image block")
	}

	// A stale failure after a successful render must not clobber the
	// picture (duplicate fetches can race a snapshot resync).
	m.handleImageBytesMsg(imageBytesMsg{blockID: "img-x", data: nil, err: errImageFetch})
	if img.ImageErr != "" || len(img.ImageLines) == 0 {
		t.Fatalf("stale failure overwrote rendered image: err=%q lines=%d", img.ImageErr, len(img.ImageLines))
	}

	// A fetch error degrades the block to readable text.
	img2 := &TranscriptBlock{ID: "img-y", Kind: BlockKindImage, ImageRef: ref, Done: true}
	idx.Add(img2)
	m.handleImageBytesMsg(imageBytesMsg{blockID: "img-y", data: nil, err: errImageFetch})
	if img2.ImageErr == "" {
		t.Fatal("fetch failure must set ImageErr")
	}
	if got := m.renderImage(img2); !strings.Contains(got, "unavailable") {
		t.Fatalf("fallback text missing failure reason: %q", got)
	}
}

var errImageFetch = &imageFetchError{}

type imageFetchError struct{}

func (*imageFetchError) Error() string { return "fetch failed" }

// TestImageLoadCmdsSkipsSettledBlocks verifies load scheduling is
// idempotent: rendered and failed blocks are not re-fetched.
func TestImageLoadCmdsSkipsSettledBlocks(t *testing.T) {
	ref := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("12", 32)), Size: 5}
	idx := NewBlockIndex()
	var out bytes.Buffer
	imgA := &TranscriptBlock{ID: "img-a", Kind: BlockKindImage, ImageRef: ref, Done: true}
	idx.Add(imgA)
	failed := &TranscriptBlock{ID: "img-b", Kind: BlockKindImage, ImageRef: ref, Done: true, ImageErr: "boom"}
	idx.Add(failed)
	settled := &TranscriptBlock{ID: "img-c", Kind: BlockKindImage, ImageRef: ref, Done: true, ImageLines: []string{"x"}}
	idx.Add(settled)
	m := Model{blocks: idx, images: termimage.NewKitty(&out)}
	cmds := m.imageLoadCmds()
	if len(cmds) != 1 {
		t.Fatalf("load cmds = %d, want 1 (only the pending block)", len(cmds))
	}
}

// TestToolCompletedArtifactsCreateImageBlock verifies the live path: a
// tool.completed event carrying displayable artifacts inserts pending image
// blocks (and skips non-image artifacts).
func TestToolCompletedArtifactsCreateImageBlock(t *testing.T) {
	m := NewModel(newTestController(t), "model-a", "/ws")
	var out bytes.Buffer
	m.images = termimage.NewKitty(&out)
	m.width = 120
	m.followTail = true

	imgRef := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("99", 32)), Size: 5}
	textRef := domain.ArtifactRef{ID: artifactID(t, "art_sha256_"+strings.Repeat("77", 32)), Size: 3}
	evt := toolEvent(t, runtimeevent.KindToolCompleted, runtimeevent.ToolCompletedPayload{
		CallID:   toolCallID(t, "call-9"),
		ToolName: "present_image",
		Status:   domain.ToolStatusSuccess,
		Artifacts: []domain.ArtifactRef{
			imgRef,
			{ID: textRef.ID, Size: 3, MediaType: "text/plain"},
		},
	})
	next, cmds := m.handleRuntimeEvent(evt)
	imgID := "img-" + imgRef.ID.String()
	b, ok := next.blocks.Get(imgID)
	if !ok {
		t.Fatalf("image block %s missing after tool.completed; order=%v", imgID, next.blocks.Order)
	}
	if !imageIsPending(b) {
		t.Fatalf("image block not pending: lines=%d err=%q", len(b.ImageLines), b.ImageErr)
	}
	// Non-image artifacts must not become image blocks.
	if _, ok := next.blocks.Get("img-" + textRef.ID.String()); ok {
		t.Fatal("text artifact rendered as image block")
	}
	// The load cmd chain must include the image fetch alongside waitForEvent.
	if cmds == nil {
		t.Fatal("expected a cmd (image load + event wait)")
	}
}
