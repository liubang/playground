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
	"bytes"
	"os"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

func TestFdReadyReportsAvailableData(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	if _, err := w.Write([]byte("x")); err != nil {
		t.Fatal(err)
	}
	ready, err := fdReady(r.Fd(), 50*time.Millisecond)
	if err != nil {
		t.Fatalf("fdReady error = %v", err)
	}
	if !ready {
		t.Fatal("fdReady = false with data buffered, want true")
	}
}

// TestFdReadySurvivesSignalStorm pins the EINTR regression: a bare ESC
// parks the reader in select(2) for the full fragment timeout, and Go's
// async preemption (SIGURG) used to interrupt that syscall into a fatal
// "interrupted system call" error. The retry loop must absorb it.
func TestFdReadySurvivesSignalStorm(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()

	stop := make(chan struct{})
	go func() {
		for {
			select {
			case <-stop:
				return
			default:
				// SIGURG is handled by the Go runtime (async preemption),
				// so blasting it at the process is safe; the point is that
				// it interrupts select(2) with EINTR.
				_ = unix.Kill(unix.Getpid(), unix.SIGURG)
				time.Sleep(2 * time.Millisecond)
			}
		}
	}()
	defer close(stop)

	// Nothing is ever written: the wait must run the full budget and still
	// come back clean instead of surfacing EINTR.
	start := time.Now()
	ready, err := fdReady(r.Fd(), 60*time.Millisecond)
	if err != nil {
		t.Fatalf("fdReady returned error under signal storm: %v", err)
	}
	if ready {
		t.Fatal("fdReady = true with no data written, want false")
	}
	if elapsed := time.Since(start); elapsed < 50*time.Millisecond {
		t.Fatalf("fdReady returned after %v, want the full timeout budget", elapsed)
	}
}

func inputPipe(t *testing.T) (*ansiSeqReader, *os.File) {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		r.Close()
		w.Close()
	})
	return newInputReader(r), w
}

// Regression: an SGR mouse report fragmented with more than the
// human-ESC budget between pieces (IPC-fronted terminals put tens of
// milliseconds between fragments) used to be forwarded early, and —
// shorter than the parser's 6-byte mouse-probe minimum — leaked into the
// composer as literal text ("[<6"). The reader must hold the incomplete
// tail until the rest arrives.
func TestAnsiSeqReaderReassemblesSlowMouseFragment(t *testing.T) {
	reader, w := inputPipe(t)
	event := []byte("\x1b[<65;47;16M")
	go func() {
		_, _ = w.Write(event[:4]) // "\x1b[<6"
		time.Sleep(150 * time.Millisecond)
		_, _ = w.Write(event[4:])
	}()

	buf := make([]byte, 64)
	n, err := reader.Read(buf)
	if err != nil {
		t.Fatalf("Read error = %v", err)
	}
	if got := buf[:n]; !bytes.Equal(got, event) {
		t.Fatalf("Read forwarded %q, want the reassembled %q", got, event)
	}
}

// A bare ESC press must still surface within the human-key budget: the
// generous sequence hold must not apply to a lone ESC byte.
func TestAnsiSeqReaderLoneEscapeStaysSnappy(t *testing.T) {
	reader, w := inputPipe(t)
	if _, err := w.Write([]byte("\x1b")); err != nil {
		t.Fatal(err)
	}

	start := time.Now()
	buf := make([]byte, 8)
	n, err := reader.Read(buf)
	if err != nil {
		t.Fatalf("Read error = %v", err)
	}
	if elapsed := time.Since(start); elapsed > 500*time.Millisecond {
		t.Fatalf("lone ESC held for %v, want roughly the escape-key budget", elapsed)
	}
	if n != 1 || buf[0] != 0x1b {
		t.Fatalf("Read forwarded %q, want a lone ESC", buf[:n])
	}
}

// Bytes that do not fit into the caller's buffer must be preserved for
// the next Read, not dropped (a fast scroll burst can exceed the parser's
// 256-byte read buffer).
func TestAnsiSeqReaderPreservesOverflow(t *testing.T) {
	reader, w := inputPipe(t)
	event := []byte("\x1b[<65;47;16M\x1b[<64;47;16M") // two reports, 26 bytes
	if _, err := w.Write(event); err != nil {
		t.Fatal(err)
	}

	var got []byte
	buf := make([]byte, 8) // deliberately smaller than the input
	for len(got) < len(event) {
		n, err := reader.Read(buf)
		if err != nil {
			t.Fatalf("Read error = %v after %d/%d bytes", err, len(got), len(event))
		}
		got = append(got, buf[:n]...)
	}
	if !bytes.Equal(got, event) {
		t.Fatalf("reassembled stream %q, want %q — bytes were dropped", got, event)
	}
}

// Text typed ahead of an incomplete sequence must be delivered
// immediately; only the incomplete tail is held.
func TestAnsiSeqReaderForwardsCompletePrefix(t *testing.T) {
	reader, w := inputPipe(t)
	tail := []byte("\x1b[<6")
	if _, err := w.Write(append([]byte("hi"), tail...)); err != nil {
		t.Fatal(err)
	}

	buf := make([]byte, 8)
	n, err := reader.Read(buf)
	if err != nil {
		t.Fatalf("Read error = %v", err)
	}
	if got := string(buf[:n]); got != "hi" {
		t.Fatalf("Read forwarded %q, want the complete prefix %q", got, "hi")
	}
}
