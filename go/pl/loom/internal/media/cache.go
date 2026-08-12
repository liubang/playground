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
// Created: 2026/08/12

package media

import (
	"container/list"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// deriveCacheCap bounds the total payload held by the process-level derive
// cache (same order as codex's 64MB image cache).
const deriveCacheCap = 64 << 20

// deriveCache memoizes Derive results keyed by artifact content hash. Keys
// are content-addressed, so a process-global cache is always correct: the
// same key can never map to different bytes.
var deriveCache = newImageCache(deriveCacheCap)

type cacheEntry struct {
	key  string
	img  domain.ImageContent
	size int
}

// imageCache is a byte-bounded LRU for derived images.
type imageCache struct {
	mu    sync.Mutex
	cap   int
	total int
	ll    *list.List // front = most recently used
	items map[string]*list.Element
}

func newImageCache(capBytes int) *imageCache {
	return &imageCache{cap: capBytes, ll: list.New(), items: make(map[string]*list.Element)}
}

func (c *imageCache) get(key string) (domain.ImageContent, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	el, ok := c.items[key]
	if !ok {
		return domain.ImageContent{}, false
	}
	c.ll.MoveToFront(el)
	return el.Value.(cacheEntry).img, true
}

func (c *imageCache) put(key string, img domain.ImageContent) {
	size := len(img.Data)
	c.mu.Lock()
	defer c.mu.Unlock()
	if el, ok := c.items[key]; ok {
		c.total -= el.Value.(cacheEntry).size
		c.ll.Remove(el)
		delete(c.items, key)
	}
	// An entry larger than the whole cache is never stored.
	if size > c.cap {
		return
	}
	for c.total+size > c.cap && c.ll.Len() > 0 {
		back := c.ll.Back()
		c.total -= back.Value.(cacheEntry).size
		delete(c.items, back.Value.(cacheEntry).key)
		c.ll.Remove(back)
	}
	el := c.ll.PushFront(cacheEntry{key: key, img: img, size: size})
	c.items[key] = el
	c.total += size
}
