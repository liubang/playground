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
// Created: 2026/07/26

package ui

import (
	"fmt"
	"os"
	"time"

	"golang.org/x/sys/unix"
)

// escapeKeyTimeout bounds how long a LONE ESC byte is held while
// waiting for bytes that would turn it into a sequence: a human Escape
// press must stay snappy.
const escapeKeyTimeout = 50 * time.Millisecond

// sequenceTimeout bounds how long an incomplete escape SEQUENCE (two or
// more bytes, so provably not a bare keypress) is held while waiting for
// its tail. IPC-fronted terminals (IDE embedded ones) deliver mouse
// reports in fragments tens of milliseconds apart; the human-ESC budget
// would forward a fragment like "\x1b[<6" early, and — being shorter
// than the parser's 6-byte mouse-probe minimum — it degrades into
// alt+"[" plus literal runes in the composer. A genuinely severed
// sequence is rare, so the budget is generous.
const sequenceTimeout = 2 * time.Second

// debugInputDumpEnv names the variable that, when set to a file path,
// makes the input reader dump every byte it forwards, together with
// boundary decisions. It exists for diagnosing terminal-specific input
// anomalies in the field and is off by default.
const debugInputDumpEnv = "LOOM_DEBUG_INPUT_DUMP"

// ansiSeqReader reassembles escape sequences that terminals deliver in
// fragments. Bubble Tea's parser (all v1 releases) reports a lone ESC
// byte as the Escape key immediately; when a mouse report such as
// "\x1b[<65;47;16M" arrives split after the ESC, the tail lands in the
// composer as text ("[<65;47;16M…"). Time-based debouncing cannot fix
// this — IPC-fronted terminals (IDE embedded ones) may put tens of
// milliseconds between fragments. This reader instead understands the
// *shape* of ANSI sequences (CSI, SS3, OSC, X10 mouse, Alt+char) and
// holds back an incomplete tail until the rest arrives, regardless of
// how long it takes (bounded by escapeKeyTimeout for a lone ESC and by
// sequenceTimeout for a sequence in progress, as escape hatches).
type ansiSeqReader struct {
	f       *os.File
	pending []byte
	dump    *os.File
}

// newInputReader builds the stdin reader for the TUI, honoring the
// debug dump env var.
func newInputReader(f *os.File) *ansiSeqReader {
	r := &ansiSeqReader{f: f}
	if path := os.Getenv(debugInputDumpEnv); path != "" {
		if dump, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600); err == nil {
			r.dump = dump
			fmt.Fprintf(dump, "# ansiSeqReader active\n")
		}
	}
	return r
}

func (r *ansiSeqReader) Read(p []byte) (int, error) {
	for {
		n, err := r.readBuffered(p)
		if n > 0 || err != nil {
			return n, err
		}
		// (0, nil): everything available is an incomplete sequence tail
		// that we keep holding; block again for the rest of it.
	}
}

func (r *ansiSeqReader) readBuffered(p []byte) (int, error) {
	if len(r.pending) > 0 {
		// Drain buffered bytes before touching the file again: pending
		// may already hold complete sequences (e.g. what did not fit into
		// the caller's buffer last time) that must not wait behind a
		// blocking read.
		if m := r.forwardComplete(p); m > 0 {
			return m, nil
		}
		// The whole buffer is one incomplete tail: wait for the rest. A
		// lone ESC gets the short human-key budget; anything longer is
		// provably a sequence in progress and gets a generous one.
		timeout := escapeKeyTimeout
		if len(r.pending) > 1 {
			timeout = sequenceTimeout
		}
		ready, serr := fdReady(r.f.Fd(), timeout)
		if serr != nil {
			return 0, serr
		}
		if !ready {
			// Escape hatch: forward the bytes anyway so a manual ESC
			// never gets stuck; the parser's lone-ESC behavior is the
			// best we can do for a truly severed sequence.
			m := copy(p, r.pending)
			r.pending = append([]byte(nil), r.pending[m:]...)
			r.debugf("-> fwd %d %q (timeout, incomplete)\n", m, p[:m])
			return m, nil
		}
	}
	scratch := make([]byte, 256)
	n, err := r.f.Read(scratch)
	if n > 0 {
		r.debugf("<- raw %d %q\n", n, scratch[:n])
		r.pending = append(r.pending, scratch[:n]...)
	}
	if len(r.pending) == 0 {
		return 0, err
	}
	// (0, nil) means everything available is an incomplete sequence
	// tail: Read loops and the wait path above handles it from here.
	return r.forwardComplete(p), nil
}

// forwardComplete copies the complete prefix of pending into p and keeps
// the remainder buffered — bytes that do not fit into p are never
// dropped. It returns 0 when the whole pending buffer is one incomplete
// escape-sequence tail.
func (r *ansiSeqReader) forwardComplete(p []byte) int {
	// splitCompletePrefix reports tailStart == len(buf) when the buffer
	// ends on a sequence boundary, so tailStart alone selects the
	// forwardable prefix in both the complete and the held-tail case.
	_, tailStart := splitCompletePrefix(r.pending)
	if tailStart == 0 {
		return 0
	}
	m := copy(p, r.pending[:tailStart])
	r.pending = append([]byte(nil), r.pending[m:]...)
	r.debugf("-> fwd %d %q\n", m, p[:m])
	return m
}

func (r *ansiSeqReader) debugf(format string, args ...any) {
	if r.dump != nil {
		fmt.Fprintf(r.dump, format, args...)
	}
}

// splitCompletePrefix reports whether buf ends on a complete escape
// sequence boundary. When false, tailStart is the index where the
// incomplete trailing sequence begins.
func splitCompletePrefix(buf []byte) (complete bool, tailStart int) {
	last := -1
	for i := len(buf) - 1; i >= 0; i-- {
		if buf[i] == 0x1b {
			last = i
			break
		}
	}
	if last < 0 {
		return true, len(buf)
	}
	if seqComplete(buf[last:]) {
		return true, len(buf)
	}
	return false, last
}

// seqComplete reports whether s, which starts with ESC, forms a complete
// escape sequence (or an Alt-modified keypress).
func seqComplete(s []byte) bool {
	if len(s) == 1 {
		return false // lone ESC so far
	}
	switch s[1] {
	case '[': // CSI: params (0x30-0x3f)*, intermediates (0x20-0x2f)*, final (0x40-0x7e)
		// X10 mouse report: ESC [ M + 3 bytes, the historical form.
		if len(s) >= 3 && s[2] == 'M' {
			return len(s) >= 6
		}
		i := 2
		for i < len(s) && s[i] >= 0x30 && s[i] <= 0x3f {
			i++
		}
		for i < len(s) && s[i] >= 0x20 && s[i] <= 0x2f {
			i++
		}
		if i >= len(s) {
			return false // no final byte yet
		}
		return s[i] >= 0x40 && s[i] <= 0x7e
	case ']': // OSC: content until BEL or ST (ESC \)
		for i := 2; i < len(s); i++ {
			if s[i] == 0x07 {
				return true
			}
			if s[i] == 0x1b {
				return i+1 < len(s) && s[i+1] == '\\'
			}
		}
		return false
	case 'O': // SS3: ESC O + one byte
		return len(s) >= 3
	default: // Alt+char
		return true
	}
}

// fdReady reports whether fd has input available within d, using select(2).
// Go's async preemption (SIGURG) interrupts the syscall regularly, so an
// EINTR is retried with the remaining budget instead of surfacing as a
// fatal read error — a bare ESC press would otherwise kill the program.
func fdReady(fd uintptr, d time.Duration) (bool, error) {
	deadline := time.Now().Add(d)
	for {
		remaining := time.Until(deadline)
		if remaining < 0 {
			remaining = 0
		}
		var set unix.FdSet
		set.Set(int(fd))
		tv := unix.NsecToTimeval(remaining.Nanoseconds())
		n, err := unix.Select(int(fd)+1, &set, nil, nil, &tv)
		if err == unix.EINTR {
			if !time.Now().Before(deadline) {
				return false, nil
			}
			continue
		}
		return n > 0, err
	}
}
