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
// Created: 2026/08/04

package server

import (
	"errors"
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

// ErrDataDirLocked reports that another loom process already owns the data
// directory (docs/SERVE_DESIGN.md §3.2, DESIGN.md §31 排他锁).
var ErrDataDirLocked = errors.New("data directory is locked by another loom process")

// DataDirLock is an exclusive flock on <datadir>/loom.lock: at most one
// serve process may own a data directory.
type DataDirLock struct {
	file *os.File
}

// AcquireDataDirLock takes the exclusive, non-blocking flock on
// <dir>/loom.lock. ErrDataDirLocked when another process holds it.
func AcquireDataDirLock(dir string) (*DataDirLock, error) {
	path := dir + string(os.PathSeparator) + "loom.lock"
	// #nosec G304 -- the path is the resolved loom data directory, not user input.
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open lock file: %w", err)
	}
	if err := unix.Flock(int(file.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		_ = file.Close()
		if errors.Is(err, unix.EWOULDBLOCK) || errors.Is(err, unix.EAGAIN) {
			return nil, ErrDataDirLocked
		}
		return nil, fmt.Errorf("flock: %w", err)
	}
	return &DataDirLock{file: file}, nil
}

// Release drops the lock and closes the file.
func (l *DataDirLock) Release() error {
	if l == nil || l.file == nil {
		return nil
	}
	_ = unix.Flock(int(l.file.Fd()), unix.LOCK_UN)
	return l.file.Close()
}

// removeSocket deletes a stale UDS socket file (safe to call when the
// data-dir lock proves no live owner).
func removeSocket(path string) error {
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("remove stale socket: %w", err)
	}
	return nil
}

// chmodSocket restricts a UDS socket to the owner (docs/SERVE_DESIGN.md §6).
func chmodSocket(path string) error {
	if err := os.Chmod(path, 0o600); err != nil {
		return fmt.Errorf("secure socket: %w", err)
	}
	return nil
}
