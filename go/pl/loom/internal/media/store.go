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
	"context"
	"fmt"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// StoreImage persists raw image bytes in the artifact store and returns the
// reference with its sniffed media type filled in. It is the shared ingress
// helper for every image source (view_image, generate_image, browser
// screenshots, user attachments): the media type is always SNIFFED from the
// bytes, and a truncated (oversized) write is an error — a partial image is
// worthless, unlike a truncated text log.
func StoreImage(ctx context.Context, store domain.ArtifactStore, raw []byte) (domain.ArtifactRef, error) {
	if store == nil {
		return domain.ArtifactRef{}, domain.NewError(domain.ErrInvalidInput, "media: artifact store is required")
	}
	mediaType := SniffImageType(raw)
	if mediaType == "" {
		return domain.ArtifactRef{}, domain.NewError(domain.ErrInvalidInput,
			"media: not a supported image (want png, jpeg, gif, or webp magic bytes)")
	}
	stage, err := store.Begin(ctx)
	if err != nil {
		return domain.ArtifactRef{}, err
	}
	if _, err := stage.Write(raw); err != nil {
		_ = stage.Abort()
		return domain.ArtifactRef{}, err
	}
	if stage.Truncated() {
		_ = stage.Abort()
		return domain.ArtifactRef{}, domain.NewError(domain.ErrBudget,
			fmt.Sprintf("media: image is %d bytes, exceeding the artifact store limit", len(raw)))
	}
	ref, err := stage.Commit(ctx)
	if err != nil {
		return domain.ArtifactRef{}, err
	}
	ref.MediaType = mediaType
	return ref, nil
}
