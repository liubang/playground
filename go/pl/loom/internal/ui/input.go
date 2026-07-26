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

// fragmentTimeout bounds how long the reader waits for the tail of an
// incomplete escape sequence before giving up and forwarding the bytes
// as-is. It only ever delays a genuine manual ESC press; sequence
// fragments from any half-decent terminal arrive far sooner.
const fragmentTimeout = 50 * time.Millisecond

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
// how long it takes (bounded by fragmentTimeout as an escape hatch).
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
	scratch := make([]byte, 256)
	n, err := r.f.Read(scratch)
	if n > 0 {
		r.debugf("<- raw %d %q\n", n, scratch[:n])
		r.pending = append(r.pending, scratch[:n]...)
	}
	if len(r.pending) == 0 {
		return 0, err
	}
	complete, tailStart := splitCompletePrefix(r.pending)
	if complete {
		out := r.pending
		r.pending = nil
		m := copy(p, out)
		r.debugf("-> fwd %d %q\n", m, p[:m])
		return m, nil
	}
	// The tail is incomplete: hold it and forward the complete prefix
	// when there is one.
	if tailStart > 0 {
		m := copy(p, r.pending[:tailStart])
		r.pending = append([]byte(nil), r.pending[tailStart:]...)
		r.debugf("-> fwd %d %q (tail %q held)\n", m, p[:m], r.pending)
		return m, nil
	}
	// The whole buffer is one incomplete tail: wait briefly for the rest.
	ready, serr := fdReady(r.f.Fd(), fragmentTimeout)
	if serr != nil {
		return 0, serr
	}
	if !ready {
		// Escape hatch: forward the bytes anyway so a manual ESC never
		// gets stuck; the parser's lone-ESC behavior is the best we can
		// do for a truly severed sequence.
		m := copy(p, r.pending)
		r.pending = nil
		r.debugf("-> fwd %d %q (timeout, incomplete)\n", m, p[:m])
		return m, nil
	}
	return 0, nil
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
func fdReady(fd uintptr, d time.Duration) (bool, error) {
	var set unix.FdSet
	set.Set(int(fd))
	tv := unix.NsecToTimeval(d.Nanoseconds())
	n, err := unix.Select(int(fd)+1, &set, nil, nil, &tv)
	return n > 0, err
}
