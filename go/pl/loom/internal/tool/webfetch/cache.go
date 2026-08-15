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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
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

// responseCache is the shared bounded LRU cache (REVIEW N1), typed to the
// fetch payload.
type responseCache = toolkit.ResponseCache[cachedResponse]

func newResponseCache(maxEntries, maxBodyBytes int, ttl time.Duration, now func() time.Time) *responseCache {
	return toolkit.NewResponseCache[cachedResponse](maxEntries, maxBodyBytes, ttl, now)
}
