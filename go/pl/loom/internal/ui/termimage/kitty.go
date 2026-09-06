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

// Package termimage renders images inline in image-capable terminals. The
// only implemented protocol is kitty's (kitty and ghostty), using its
// Unicode-placeholder scheme: the image is transmitted once out-of-band, and
// the transcript carries plain placeholder CELLS (U+10EEEE + combining
// diacritics), so scrolls and full-frame repaints move the picture with the
// text at zero extra protocol traffic. Terminals without support get no
// rendering at all (the caller keeps its text placeholder).
package termimage

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"image"
	_ "image/gif" // decode support: present_image accepts gif
	_ "image/jpeg"
	"image/png"
	"io"
	"log"
	"strings"
	"sync"
)

const (
	apcPrefix = "\x1b_G"
	apcSuffix = "\x1b\\"
	// placeholderCell is the kitty Unicode image placeholder (PUA).
	placeholderCell = '\U0010EEEE'
	// chunkSize is the protocol's per-transmission payload limit.
	chunkSize = 4096
	// maxImageBytes caps the artifact we bother transmitting (kitty has to
	// hold the decoded bitmap in RAM; very large sources are simply skipped).
	maxImageBytes = 12 << 20
	// maxImages bounds live transmitted images; the oldest is deleted
	// (a=d,d=i) once the cap is exceeded.
	maxImages = 64
)

// Kitty renders images on kitty-protocol terminals. Render is idempotent per
// key: the bitmap is transmitted once, later calls only rebuild the
// placeholder text. Safe for concurrent use.
type Kitty struct {
	mu     sync.Mutex
	out    io.Writer
	debug  *log.Logger // optional transmission trace (LOOM_IMAGE_DEBUG)
	nextID uint32
	sent   map[string]uint32 // key -> kitty image id (transmitted)
	order  []string          // FIFO for eviction
}

// NewKitty returns a renderer writing kitty escape sequences to out (the
// process stdout, i.e. the same writer the TUI frames go to).
func NewKitty(out io.Writer) *Kitty {
	return &Kitty{out: out, nextID: 1, sent: map[string]uint32{}}
}

// SetDebugLogger attaches a trace logger for transmission diagnostics.
// The terminal answers nothing (q=2), so this log is the only record of
// what was actually handed to the terminal.
func (k *Kitty) SetDebugLogger(l *log.Logger) { k.debug = l }

// Render ensures raw (a png/jpeg/gif image, keyed by key) is known to the
// terminal and returns the placeholder lines displaying it within maxCols
// cells of width. The lines are styled text meant to be inserted into the
// transcript verbatim — each cell's foreground color encodes the image id.
func (k *Kitty) Render(key string, raw []byte, maxCols int) ([]string, error) {
	if len(raw) == 0 || len(raw) > maxImageBytes {
		return nil, fmt.Errorf("image size %d out of range", len(raw))
	}
	if maxCols < 1 {
		return nil, fmt.Errorf("maxCols must be positive")
	}
	pngData, w, h, err := toPNG(raw)
	if err != nil {
		return nil, err
	}
	cols, rows := fitCells(w, h, maxCols, 24)
	// Placeholder cells encode row/column via the diacritics table, whose
	// size is the hard cap for either dimension regardless of maxCols.
	if cols > len(diacritics)-1 {
		cols = len(diacritics) - 1
	}
	if rows > len(diacritics)-1 {
		rows = len(diacritics) - 1
	}

	k.mu.Lock()
	defer k.mu.Unlock()
	id, ok := k.sent[key]
	if !ok {
		id = k.nextID
		k.nextID++
		if err := k.transmitLocked(id, pngData, cols, rows); err != nil {
			return nil, err
		}
		k.sent[key] = id
		k.order = append(k.order, key)
		k.evictLocked()
		if k.debug != nil {
			k.debug.Printf("transmit key=%q id=%d src=%dx%d cells=%dx%d bytes=%d",
				key, id, w, h, cols, rows, len(pngData))
		}
	}
	return placeholderLines(id, cols, rows), nil
}

// lockableTermWriter is implemented by outputs that can serialize a whole
// multi-write transmission against concurrent frame writes (bubbletea's
// renderer shares the same fd). While the lock is held, writes must go
// through UnlockedWriter to avoid re-entering the lock.
type lockableTermWriter interface {
	sync.Locker
	UnlockedWriter() io.Writer
}

// transmitLocked streams the bitmap with a combined virtual placement
// (a=T + U=1 + c/r): the terminal stores the image and creates an invisible
// rectangle prototype; actual display happens via the placeholder cells.
// q=2 suppresses terminal responses so nothing leaks into the input stream.
//
// The whole chunked transfer holds the terminal lock when the writer
// provides one: each chunk is a complete APC sequence, but a partial write
// retried mid-sequence would let a renderer frame splice into it and the
// terminal would silently drop the image.
func (k *Kitty) transmitLocked(id uint32, data []byte, cols, rows int) error {
	out := k.out
	if lw, ok := k.out.(lockableTermWriter); ok {
		lw.Lock()
		defer lw.Unlock()
		out = lw.UnlockedWriter()
	}
	b64 := base64.StdEncoding.EncodeToString(data)
	for off := 0; off < len(b64); off += chunkSize {
		end := min(off+chunkSize, len(b64))
		more := end < len(b64)
		var b strings.Builder
		b.WriteString(apcPrefix)
		if off == 0 {
			fmt.Fprintf(&b, "a=T,t=d,f=100,i=%d,c=%d,r=%d,U=1,q=2,", id, cols, rows)
		}
		if more {
			b.WriteString("m=1")
		} else {
			b.WriteString("m=0")
		}
		b.WriteByte(';')
		b.WriteString(b64[off:end])
		b.WriteString(apcSuffix)
		if _, err := io.WriteString(out, b.String()); err != nil {
			return err
		}
	}
	return nil
}

// evictLocked deletes the oldest image from the terminal once past the cap.
func (k *Kitty) evictLocked() {
	for len(k.order) > maxImages {
		oldest := k.order[0]
		k.order = k.order[1:]
		id := k.sent[oldest]
		delete(k.sent, oldest)
		fmt.Fprintf(k.out, "%sa=d,d=i,I=%d,q=2%s", apcPrefix, id, apcSuffix)
	}
}

// placeholderLines builds rows lines of cols placeholder cells each. Every
// cell is U+10EEEE + row diacritic + column diacritic, with the image id in
// the 24-bit foreground color. Diacritics are emitted unconditionally (no
// reliance on the inheritance rules, which break under horizontal clipping).
func placeholderLines(id uint32, cols, rows int) []string {
	if id > 0xFFFFFF {
		id = 0xFFFFFF // 24-bit fg color space; ids never reach this in practice
	}
	sgr := fmt.Sprintf("\x1b[38;2;%d;%d;%dm", id>>16&0xFF, id>>8&0xFF, id&0xFF)
	lines := make([]string, rows)
	for row := 0; row < rows; row++ {
		var b strings.Builder
		b.WriteString(sgr)
		for col := 0; col < cols; col++ {
			b.WriteRune(placeholderCell)
			b.WriteRune(diacritic(row))
			b.WriteRune(diacritic(col))
		}
		b.WriteString("\x1b[39m")
		lines[row] = b.String()
	}
	return lines
}

func diacritic(n int) rune {
	if n >= 0 && n < len(diacritics) {
		return diacritics[n]
	}
	// Beyond the table the terminal falls back to inheriting from the left
	// neighbor — acceptable only because dims are clamped far below this.
	return diacritics[len(diacritics)-1]
}

// fitCells picks a cell rectangle preserving the image aspect ratio, assuming
// the conventional 1:2 cell geometry (a cell is twice as tall as wide).
// kitty fits the image inside the rectangle preserving aspect itself, so
// approximation here only affects how much padding surrounds the picture.
func fitCells(w, h, maxCols, maxRows int) (cols, rows int) {
	if w <= 0 || h <= 0 || maxCols <= 0 || maxRows <= 0 {
		return 0, 0
	}
	cols = maxCols
	rows = max(1, int(float64(h)/float64(w)*float64(cols)*0.5+0.5))
	if rows > maxRows {
		rows = maxRows
		cols = max(1, int(float64(rows)*float64(w)/float64(h)*2+0.5))
	}
	if cols > maxCols {
		cols = maxCols
	}
	return cols, rows
}

// toPNG returns PNG bytes plus the pixel dimensions. PNG input passes through
// untouched; jpeg/gif are decoded and re-encoded (gif: first frame).
func toPNG(raw []byte) (pngData []byte, w, h int, err error) {
	cfg, format, err := image.DecodeConfig(bytes.NewReader(raw))
	if err != nil {
		return nil, 0, 0, fmt.Errorf("decode image config: %w", err)
	}
	if format == "png" {
		return raw, cfg.Width, cfg.Height, nil
	}
	img, _, err := image.Decode(bytes.NewReader(raw))
	if err != nil {
		return nil, 0, 0, fmt.Errorf("decode image: %w", err)
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return nil, 0, 0, fmt.Errorf("re-encode png: %w", err)
	}
	return buf.Bytes(), cfg.Width, cfg.Height, nil
}
