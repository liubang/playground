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
// Created: 2026/07/24

package workspace

import "sync"

// FileStateBook tracks content hashes of files the agent has recently read or
// written, letting edit-style tools detect external modifications without
// forcing the model to carry an expected_hash. It is shared by read_file
// (which records hashes) and edit (which checks for drift). The book is
// process-local and never persisted: after a restart, edits require a fresh
// read, which is the fail-closed default.
type FileStateBook struct {
	mu     sync.Mutex
	hashes map[string]string
}

// NewFileStateBook creates an empty book.
func NewFileStateBook() *FileStateBook {
	return &FileStateBook{hashes: make(map[string]string)}
}

// Record stores the latest known hash for an absolute path.
func (b *FileStateBook) Record(abs, hash string) {
	if b == nil || abs == "" || hash == "" {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	b.hashes[abs] = hash
}

// Stale reports whether the file at abs is externally modified relative to
// the last recorded hash. known is false when the file was never recorded;
// stale is false when unknown or unchanged.
func (b *FileStateBook) Stale(abs, currentHash string) (known, stale bool) {
	if b == nil {
		return false, false
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	recorded, ok := b.hashes[abs]
	if !ok {
		return false, false
	}
	return true, recorded != currentHash
}

// Forget drops the recorded state for abs (e.g. after the file is deleted).
func (b *FileStateBook) Forget(abs string) {
	if b == nil {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	delete(b.hashes, abs)
}
