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

package termimage

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"strings"
	"sync"
	"testing"
)

func testPNG(t *testing.T, w, h int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.Set(x, y, color.RGBA{uint8(x), uint8(y), 128, 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("encode: %v", err)
	}
	return buf.Bytes()
}

func TestDetect(t *testing.T) {
	cases := []struct {
		name string
		env  map[string]string
		want Protocol
	}{
		{"kitty term", map[string]string{"TERM": "xterm-kitty"}, ProtocolKitty},
		{"kitty window id", map[string]string{"KITTY_WINDOW_ID": "1", "TERM": "xterm-256color"}, ProtocolKitty},
		{"ghostty", map[string]string{"TERM": "xterm-ghostty"}, ProtocolKitty},
		{"wezterm excluded", map[string]string{"TERM_PROGRAM": "WezTerm"}, ProtocolNone},
		{"tmux blocks", map[string]string{"TERM": "xterm-kitty", "TMUX": "/tmp/tmux"}, ProtocolNone},
		{"no color disables", map[string]string{"TERM": "xterm-kitty", "NO_COLOR": "1"}, ProtocolNone},
		{"empty no color ignored", map[string]string{"TERM": "xterm-kitty", "NO_COLOR": ""}, ProtocolKitty},
		{"plain xterm", map[string]string{"TERM": "xterm-256color"}, ProtocolNone},
		{"iterm unsupported", map[string]string{"TERM_PROGRAM": "iTerm.app"}, ProtocolNone},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := Detect(func(k string) string { return tc.env[k] })
			if got != tc.want {
				t.Fatalf("Detect() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestRenderTransmitOnceAndPlaceholderShape(t *testing.T) {
	var out bytes.Buffer
	k := NewKitty(&out)
	raw := testPNG(t, 40, 20)

	lines, err := k.Render("k1", raw, 20)
	if err != nil {
		t.Fatalf("Render: %v", err)
	}
	// 40x20 px, 1:2 cell geometry: 20 cols -> 5 rows.
	if len(lines) != 5 {
		t.Fatalf("rows = %d, want 5", len(lines))
	}
	for i, line := range lines {
		if !strings.Contains(line, "\x1b[38;2;0;0;1m") {
			t.Fatalf("line %d missing image-id fg color", i)
		}
		if got := strings.Count(line, "\U0010EEEE"); got != 20 {
			t.Fatalf("line %d placeholder cells = %d, want 20", i, got)
		}
		if !strings.HasSuffix(line, "\x1b[39m") {
			t.Fatalf("line %d missing fg reset", i)
		}
	}
	// First line, first cell: row 0 -> U+0305, col 0 -> U+0305.
	if !strings.Contains(lines[0], "\U0010EEEE\u0305\u0305") {
		t.Fatalf("first cell diacritics wrong: %q", lines[0])
	}
	// Second row's first cell must carry row diacritic U+030D (row 1).
	if !strings.Contains(lines[1], "\U0010EEEE\u030D\u0305") {
		t.Fatalf("row-1 cell diacritics wrong: %q", lines[1])
	}

	// Transmission happened exactly once for this key...
	firstTx := out.String()
	if !strings.Contains(firstTx, "a=T,t=d,f=100,i=1,c=20,r=5,U=1,q=2,m=") {
		t.Fatalf("first chunk keys wrong: %q", firstTx[:min(120, len(firstTx))])
	}
	// ...and the second Render call adds nothing new.
	before := out.Len()
	if _, err := k.Render("k1", raw, 20); err != nil {
		t.Fatalf("second Render: %v", err)
	}
	if out.Len() != before {
		t.Fatalf("re-render retransmitted: out grew by %d bytes", out.Len()-before)
	}
}

func TestTransmitChunking(t *testing.T) {
	var out bytes.Buffer
	k := NewKitty(&out)
	// Random noise defeats png compression: the base64 payload spans chunks.
	img := image.NewRGBA(image.Rect(0, 0, 64, 64))
	seed := uint32(42)
	for i := range img.Pix {
		seed = seed*1664525 + 1013904223
		img.Pix[i] = byte(seed >> 24)
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("encode: %v", err)
	}
	raw := buf.Bytes()
	if _, err := k.Render("big", raw, 10); err != nil {
		t.Fatalf("Render: %v", err)
	}
	s := out.String()
	chunks := strings.Count(s, "m=1")
	last := strings.Contains(s, "m=0;")
	if chunks < 2 || !last {
		t.Fatalf("chunking broken: %d continuation chunks, final present=%v", chunks, last)
	}
	// Every chunk's payload must be <= 4096 base64 chars.
	for _, seg := range strings.Split(s, "\x1b_G")[1:] {
		body := strings.TrimSuffix(seg, "\x1b\\")
		payload := body[strings.IndexByte(body, ';')+1:]
		if len(payload) > chunkSize {
			t.Fatalf("chunk payload %d > %d", len(payload), chunkSize)
		}
		if _, err := base64.StdEncoding.DecodeString(payload); err != nil {
			t.Fatalf("chunk payload not valid base64: %v", err)
		}
	}
}

func TestRenderRejectsOversize(t *testing.T) {
	var out bytes.Buffer
	k := NewKitty(&out)
	if _, err := k.Render("huge", make([]byte, maxImageBytes+1), 20); err == nil {
		t.Fatal("expected error for oversize image")
	}
}

func TestFitCells(t *testing.T) {
	cases := []struct {
		w, h, maxC, maxR int
		wantC, wantR     int
	}{
		{200, 100, 80, 24, 80, 20}, // landscape: width-bound
		{100, 400, 80, 24, 12, 24}, // portrait: height-bound
		{40, 20, 20, 24, 20, 5},    // small
		{0, 0, 80, 24, 0, 0},       // degenerate
	}
	for _, tc := range cases {
		c, r := fitCells(tc.w, tc.h, tc.maxC, tc.maxR)
		if c != tc.wantC || r != tc.wantR {
			t.Errorf("fitCells(%d,%d,%d,%d) = (%d,%d), want (%d,%d)",
				tc.w, tc.h, tc.maxC, tc.maxR, c, r, tc.wantC, tc.wantR)
		}
	}
}

// TestRenderClampsDiacriticsTable verifies a caller-supplied maxCols larger
// than the diacritics table cannot produce out-of-range placeholder cells.
func TestRenderClampsDiacriticsTable(t *testing.T) {
	var out bytes.Buffer
	k := NewKitty(&out)
	lines, err := k.Render("wide", testPNG(t, 4000, 100), 1000)
	if err != nil {
		t.Fatalf("Render: %v", err)
	}
	for _, line := range lines {
		if n := strings.Count(line, "\U0010EEEE"); n > len(diacritics)-1 {
			t.Fatalf("placeholder row width %d exceeds diacritics table", n)
		}
	}
}

// TestTransmitHoldsWriterLock verifies a lockable writer stays locked for
// the whole chunked transfer and the chunks go through the unlocked side.
func TestTransmitHoldsWriterLock(t *testing.T) {
	w := &spyLockWriter{}
	k := NewKitty(w)
	if _, err := k.Render("big", testPNG(t, 400, 300), 40); err != nil {
		t.Fatalf("Render: %v", err)
	}
	if w.locks != 1 || w.unlocks != 1 {
		t.Fatalf("locks=%d unlocks=%d, want 1/1", w.locks, w.unlocks)
	}
	if w.lockedWrites == 0 || w.plainWrites != 0 {
		t.Fatalf("lockedWrites=%d plainWrites=%d, want all writes via UnlockedWriter",
			w.lockedWrites, w.plainWrites)
	}
}

// spyLockWriter records lock usage and which side the writes came through.
type spyLockWriter struct {
	mu           sync.Mutex
	locks        int
	unlocks      int
	plainWrites  int
	lockedWrites int
}

func (s *spyLockWriter) Write(p []byte) (int, error) {
	s.plainWrites++
	return len(p), nil
}

func (s *spyLockWriter) Lock() {
	s.mu.Lock()
	s.locks++
}

func (s *spyLockWriter) Unlock() {
	s.mu.Unlock()
	s.unlocks++
}

func (s *spyLockWriter) UnlockedWriter() io.Writer {
	return writerFunc(func(p []byte) (int, error) {
		if !s.mu.TryLock() {
			// The mutex is held by transmit — exactly what we want.
			s.lockedWrites++
			return len(p), nil
		}
		s.mu.Unlock()
		return 0, fmt.Errorf("unlocked write escaped the transmission lock")
	})
}

type writerFunc func(p []byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

func TestEvictionDeletesOldest(t *testing.T) {
	var out bytes.Buffer
	k := NewKitty(&out)
	raw := testPNG(t, 8, 8)
	for i := 0; i < maxImages+2; i++ {
		if _, err := k.Render(string(rune('a'+i)), raw, 4); err != nil {
			t.Fatalf("Render %d: %v", i, err)
		}
	}
	s := out.String()
	if !strings.Contains(s, "a=d,d=i,I=1,q=2") || !strings.Contains(s, "a=d,d=i,I=2,q=2") {
		t.Fatalf("oldest images not evicted: %q", s[len(s)-200:])
	}
	if len(k.sent) != maxImages {
		t.Fatalf("sent size = %d, want %d", len(k.sent), maxImages)
	}
}
