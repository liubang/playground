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
// Created: 2026/08/15

package toolkit

import (
	"strings"
	"testing"
	"time"
)

func TestResponseCacheLRUAndTTL(t *testing.T) {
	now := time.Now()
	current := now
	cache := NewResponseCache[string](2, 1<<20, time.Minute, func() time.Time { return current })

	cache.Put("a", "A", 1)
	cache.Put("b", "B", 1)
	if v, ok := cache.Get("a"); !ok || v != "A" {
		t.Fatalf("Get(a) = %q, %v; want A, true", v, ok)
	}
	// a is now most-recently-used; inserting c evicts b.
	cache.Put("c", "C", 1)
	if _, ok := cache.Get("b"); ok {
		t.Fatal("expected b to be evicted")
	}
	if v, ok := cache.Get("c"); !ok || v != "C" {
		t.Fatalf("Get(c) = %q, %v; want C, true", v, ok)
	}

	current = current.Add(2 * time.Minute)
	if _, ok := cache.Get("c"); ok {
		t.Fatal("expected c to expire after TTL")
	}
	if _, ok := cache.Get("missing"); ok {
		t.Fatal("expected miss for unknown key")
	}
}

func TestResponseCacheRefusesOversizedAndDisabled(t *testing.T) {
	now := time.Now()
	current := now

	// Oversized bodies are never stored.
	cache := NewResponseCache[string](2, 8, time.Minute, func() time.Time { return current })
	cache.Put("big", strings.Repeat("x", 16), 16)
	if _, ok := cache.Get("big"); ok {
		t.Fatal("oversized body should not be cached")
	}

	// maxEntries <= 0 disables caching entirely.
	disabled := NewResponseCache[string](0, 1<<20, time.Minute, func() time.Time { return current })
	disabled.Put("k", "v", 1)
	if _, ok := disabled.Get("k"); ok {
		t.Fatal("disabled cache should never store")
	}

	// Refresh updates the value and resets the TTL.
	refresh := NewResponseCache[string](2, 1<<20, time.Minute, func() time.Time { return current })
	refresh.Put("k", "v1", 1)
	refresh.Put("k", "v2", 1)
	current = current.Add(30 * time.Second)
	if v, ok := refresh.Get("k"); !ok || v != "v2" {
		t.Fatalf("Get(k) = %q, %v; want v2, true after refresh", v, ok)
	}
}
