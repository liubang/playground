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
	"context"
	"fmt"
	"log"
	"os"
	"strings"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// imageview.go wires inline image blocks (generate_image / present_image
// outputs) to the terminal. Capability detection happens once at startup
// (termimage.Detect): only terminals with the kitty graphics protocol get
// real rendering, everything else keeps the textual degradation below.
//
// Flow: a tool completion (or a transcript rebuild) inserts a BlockKindImage
// block carrying the artifact reference. The model schedules an async fetch
// of the artifact bytes (Client.ReadArtifact — in-proc or over the wire);
// on arrival the bytes are transmitted once to the terminal and the block
// stores the placeholder lines that actually display the picture.

// imageMaxCols caps how many terminal columns an inline image may occupy.
const imageMaxCols = 72

// imageBytesMsg carries the result of an async artifact fetch.
type imageBytesMsg struct {
	blockID string
	data    []byte
	err     error
}

// imageDebugLog traces image loading/transmission when LOOM_IMAGE_DEBUG
// names a log file; nil otherwise. The terminal never talks back (q=2), so
// this file is the ground truth for "did the bytes reach the terminal".
var imageDebugLog = func() *log.Logger {
	path := os.Getenv("LOOM_IMAGE_DEBUG")
	if path == "" {
		return nil
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return nil
	}
	return log.New(f, "image: ", log.Ltime|log.Lmicroseconds)
}()

// loadImageBytesCmd fetches one image artifact off the event loop.
func (m Model) loadImageBytesCmd(blockID string, ref domain.ArtifactRef) tea.Cmd {
	return func() tea.Msg {
		data, err := m.controller.ReadArtifact(context.Background(), ref)
		return imageBytesMsg{blockID: blockID, data: data, err: err}
	}
}

// isImageMediaType reports whether an artifact media type is (or may be) an
// image we can render. Empty (older records lack it) is allowed — content
// sniffing decides at render time.
func isImageMediaType(mt string) bool {
	switch mt {
	case "", "image/png", "image/jpeg", "image/gif", "image/webp":
		return true
	default:
		return false
	}
}

// imageIsPending reports whether a block is an image block whose artifact has
// not been loaded yet (neither rendered nor failed).
func imageIsPending(b *TranscriptBlock) bool {
	return b.Kind == BlockKindImage && !b.ImageRef.ID.IsZero() &&
		len(b.ImageLines) == 0 && b.ImageErr == ""
}

// imageLoadCmds schedules fetches for every pending image block. Idempotent:
// rendered or failed blocks are skipped. Called after events and snapshots
// that may have introduced image blocks.
func (m Model) imageLoadCmds() []tea.Cmd {
	if m.images == nil {
		return nil
	}
	var cmds []tea.Cmd
	for _, id := range m.blocks.Order {
		b, ok := m.blocks.ByID[id]
		if !ok || !imageIsPending(b) {
			continue
		}
		cmds = append(cmds, m.loadImageBytesCmd(id, b.ImageRef))
	}
	return cmds
}

// handleImageBytesMsg completes a pending image block: transmit the artifact
// bytes to the terminal and store the placeholder lines. A nil image engine
// (unsupported terminal) or any failure leaves a readable error state.
func (m Model) handleImageBytesMsg(msg imageBytesMsg) Model {
	b, ok := m.blocks.Get(msg.blockID)
	if !ok {
		return m
	}
	if imageDebugLog != nil {
		imageDebugLog.Printf("bytes block=%q len=%d err=%v images-nil=%v",
			msg.blockID, len(msg.data), msg.err, m.images == nil)
	}
	if msg.err != nil {
		// A stale failure from a duplicate fetch (snapshot resync) must not
		// overwrite an already-rendered picture.
		if len(b.ImageLines) > 0 {
			return m
		}
		b.ImageErr = fmt.Sprintf("image unavailable: %v", msg.err)
		m.blocks.touch()
		return m
	}
	if m.images == nil {
		b.ImageErr = "inline images are not supported by this terminal"
		m.blocks.touch()
		return m
	}
	// Stay inside the transcript gutter: a placeholder row wider than the
	// viewport would soft-wrap and break the cell grid the terminal uses to
	// place the picture.
	maxCols := max(1, m.width-6)
	if maxCols > imageMaxCols {
		maxCols = imageMaxCols
	}
	lines, err := m.images.Render(msg.blockID, msg.data, maxCols)
	if err != nil {
		if imageDebugLog != nil {
			imageDebugLog.Printf("render block=%q failed: %v", msg.blockID, err)
		}
		b.ImageErr = fmt.Sprintf("image render failed: %v", err)
	} else {
		if imageDebugLog != nil {
			imageDebugLog.Printf("render block=%q ok lines=%d width=%d", msg.blockID, len(lines), maxCols)
		}
		b.ImageLines = lines
		b.ImageErr = ""
	}
	m.blocks.touch()
	return m
}

// renderImage renders an image block: the placeholder cells once ready, or a
// compact text fallback while pending or after a failure.
func (m Model) renderImage(block *TranscriptBlock) string {
	switch {
	case len(block.ImageLines) > 0:
		return strings.Join(block.ImageLines, "\n")
	case block.ImageErr != "":
		return m.theme.Dim.Render("⚠ " + block.ImageErr)
	default:
		return m.theme.Dim.Render("[image…]")
	}
}

// insertImageBlock adds an image block right after the anchor block
// (its tool call) when the terminal engine is available. Callers chaining
// multiple images pass the previously returned ID as the next anchor so
// order is preserved. Returns the block ID ("" when skipped).
func (m Model) insertImageBlock(anchor string, ref domain.ArtifactRef) string {
	if m.images == nil {
		return ""
	}
	id := fmt.Sprintf("img-%s", ref.ID)
	if _, exists := m.blocks.Get(id); exists {
		return id
	}
	m.blocks.InsertAfter(anchor, &TranscriptBlock{
		ID:       id,
		Kind:     BlockKindImage,
		ImageRef: ref,
		Done:     true,
	})
	return id
}
