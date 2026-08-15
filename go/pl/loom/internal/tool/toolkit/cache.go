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
	"container/list"
	"sync"
	"time"
)

// cacheElement is one LRU node holding a value of the caller's type.
type cacheElement[T any] struct {
	key       string
	value     T
	expiresAt time.Time
}

// ResponseCache is a bounded in-process LRU with a TTL, shared by the
// web_fetch and web_search tools (REVIEW N1 — previously duplicated
// per-package "per loom convention"). The caller supplies the byte size
// of its payload at Put time so the cache itself stays payload-agnostic.
type ResponseCache[T any] struct {
	mu           sync.Mutex
	ll           *list.List
	items        map[string]*list.Element
	maxEntries   int
	maxBodyBytes int
	ttl          time.Duration
	now          func() time.Time
}

// NewResponseCache creates a bounded LRU cache. A zero or negative
// maxEntries disables caching entirely (Put becomes a no-op).
func NewResponseCache[T any](maxEntries, maxBodyBytes int, ttl time.Duration, now func() time.Time) *ResponseCache[T] {
	if now == nil {
		now = time.Now
	}
	return &ResponseCache[T]{
		ll:           list.New(),
		items:        make(map[string]*list.Element),
		maxEntries:   maxEntries,
		maxBodyBytes: maxBodyBytes,
		ttl:          ttl,
		now:          now,
	}
}

// Get returns the cached value for key, moving it to the front of the
// LRU. Expired entries are removed on access.
func (c *ResponseCache[T]) Get(key string) (T, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var zero T
	elem, ok := c.items[key]
	if !ok {
		return zero, false
	}
	entry := elem.Value.(cacheElement[T])
	if c.now().After(entry.expiresAt) {
		c.removeLocked(elem)
		return zero, false
	}
	c.ll.MoveToFront(elem)
	return entry.value, true
}

// Put stores value under key, or refreshes it when the key exists.
// Oversized payloads (bodyBytes > maxBodyBytes) are refused, and the
// LRU evicts the oldest entry once the size cap is exceeded.
func (c *ResponseCache[T]) Put(key string, value T, bodyBytes int) {
	if c.maxEntries <= 0 || bodyBytes > c.maxBodyBytes {
		return
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if elem, ok := c.items[key]; ok {
		elem.Value = cacheElement[T]{key: key, value: value, expiresAt: c.now().Add(c.ttl)}
		c.ll.MoveToFront(elem)
		return
	}
	elem := c.ll.PushFront(cacheElement[T]{key: key, value: value, expiresAt: c.now().Add(c.ttl)})
	c.items[key] = elem
	for c.ll.Len() > c.maxEntries {
		c.removeLocked(c.ll.Back())
	}
}

// removeLocked removes an element from both the list and the index.
func (c *ResponseCache[T]) removeLocked(elem *list.Element) {
	if elem == nil {
		return
	}
	c.ll.Remove(elem)
	delete(c.items, elem.Value.(cacheElement[T]).key)
}
