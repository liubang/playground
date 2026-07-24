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

package webfetch

import (
	"container/list"
	"sync"
	"time"
)

// cachedResponse holds the post-conversion body of a successful fetch. The
// body is stored before output truncation so cache hits can re-apply a
// different max_bytes budget.
type cachedResponse struct {
	FinalURL    string
	Status      int
	ContentType string
	FetchedAt   time.Time
	Body        string
}

type cacheElement struct {
	key       string
	response  cachedResponse
	expiresAt time.Time
}

// responseCache is a bounded in-process LRU with a TTL. It only stores
// successful (2xx) GET responses without a no-store directive.
type responseCache struct {
	mu           sync.Mutex
	ll           *list.List
	items        map[string]*list.Element
	maxEntries   int
	maxBodyBytes int
	ttl          time.Duration
	now          func() time.Time
}

func newResponseCache(maxEntries, maxBodyBytes int, ttl time.Duration, now func() time.Time) *responseCache {
	if now == nil {
		now = time.Now
	}
	return &responseCache{
		ll:           list.New(),
		items:        make(map[string]*list.Element),
		maxEntries:   maxEntries,
		maxBodyBytes: maxBodyBytes,
		ttl:          ttl,
		now:          now,
	}
}

func (c *responseCache) get(key string) (cachedResponse, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	elem, ok := c.items[key]
	if !ok {
		return cachedResponse{}, false
	}
	entry := elem.Value.(cacheElement)
	if c.now().After(entry.expiresAt) {
		c.removeLocked(elem)
		return cachedResponse{}, false
	}
	c.ll.MoveToFront(elem)
	return entry.response, true
}

func (c *responseCache) put(key string, resp cachedResponse) {
	if c.maxEntries <= 0 || len(resp.Body) > c.maxBodyBytes {
		return
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if elem, ok := c.items[key]; ok {
		elem.Value = cacheElement{key: key, response: resp, expiresAt: c.now().Add(c.ttl)}
		c.ll.MoveToFront(elem)
		return
	}
	elem := c.ll.PushFront(cacheElement{key: key, response: resp, expiresAt: c.now().Add(c.ttl)})
	c.items[key] = elem
	for c.ll.Len() > c.maxEntries {
		c.removeLocked(c.ll.Back())
	}
}

func (c *responseCache) removeLocked(elem *list.Element) {
	if elem == nil {
		return
	}
	c.ll.Remove(elem)
	delete(c.items, elem.Value.(cacheElement).key)
}
