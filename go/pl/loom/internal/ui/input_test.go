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
