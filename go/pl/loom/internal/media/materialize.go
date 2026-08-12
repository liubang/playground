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

// Materialize returns a wire-ready copy of messages with every image
// artifact reference resolved into a derived inline image part (see the
// package doc). The input is never mutated; messages without image
// references are returned as-is (zero-copy fast path).
//
// A reference that cannot be resolved (store read failure, undecodable
// bytes) degrades to a text placeholder instead of failing the whole model
// call — one bad image must not take down the turn (codex's error
// placeholder pattern).
//
// A nil store passes messages through unchanged: sessions without an
// artifact store (tests, hand-assembled runtimes) keep legacy behavior.
func Materialize(ctx context.Context, store domain.ArtifactStore, messages []domain.Message) []domain.Message {
	if store == nil {
		return messages
	}
	return rewriteImages(messages, func(ref domain.ArtifactRef) domain.ContentPart {
		return resolve(ctx, store, ref)
	})
}

// StripImages returns a copy of messages with every model-bound image
// reference replaced by a text note. It is the non-vision-model counterpart
// of Materialize: providers reject image parts (and user-role artifact
// parts) outright, so a model without image input must receive an explicit
// gap it can reason about (codex's "you do not support image inputs").
func StripImages(messages []domain.Message) []domain.Message {
	return rewriteImages(messages, func(ref domain.ArtifactRef) domain.ContentPart {
		return domain.ContentPart{
			Kind: domain.PartText,
			Text: fmt.Sprintf("[image %s omitted: the active model does not support image input]", ref.ID.String()),
		}
	})
}

// rewriteImages applies rewrite to every model-bound image artifact part —
// top-level message parts (user attachments) and tool-result content parts
// alike — returning the input slice untouched when nothing matches.
func rewriteImages(messages []domain.Message, rewrite func(domain.ArtifactRef) domain.ContentPart) []domain.Message {
	// out stays nil until the first change so the no-image fast path
	// returns the input untouched; once allocated it shadows the canonical
	// history, which is never mutated.
	var out []domain.Message
	for i, msg := range messages {
		derived, changed := rewriteMessageImages(msg, rewrite)
		if !changed {
			if out != nil {
				out[i] = msg
			}
			continue
		}
		if out == nil {
			out = make([]domain.Message, len(messages))
			copy(out, messages[:i])
		}
		out[i] = derived
	}
	if out == nil {
		return messages
	}
	return out
}

// rewriteMessageImages rewrites the model-bound image parts of one message,
// returning the original message unchanged when nothing matches.
func rewriteMessageImages(msg domain.Message, rewrite func(domain.ArtifactRef) domain.ContentPart) (domain.Message, bool) {
	changed := false
	parts := make([]domain.ContentPart, len(msg.Parts))
	copy(parts, msg.Parts)
	for i, part := range parts {
		switch {
		case IsModelImage(part):
			parts[i] = rewrite(*part.Artifact)
			changed = true
		case part.ToolResult != nil && hasImageArtifact(part.ToolResult.Content):
			content := make([]domain.ContentPart, len(part.ToolResult.Content))
			copy(content, part.ToolResult.Content)
			for j, c := range content {
				if IsModelImage(c) {
					content[j] = rewrite(*c.Artifact)
				}
			}
			result := *part.ToolResult
			result.Content = content
			parts[i].ToolResult = &result
			changed = true
		}
	}
	if !changed {
		return msg, false
	}
	msg.Parts = parts
	return msg, true
}

// IsModelImage reports whether the part is an image artifact reference
// bound for the model: Materialize resolves these into inline images at
// the egress, and token accounting counts their derived footprint.
// Present-only (display-only) artifacts are excluded by design — the
// model never sees them. ModelOnly is the orthogonal display-side flag
// and does not affect model binding: those images are resolved here.
func IsModelImage(part domain.ContentPart) bool {
	return part.Kind == domain.PartArtifact && !part.PresentOnly &&
		part.Artifact != nil && IsImageMediaType(part.Artifact.MediaType)
}

func hasImageArtifact(parts []domain.ContentPart) bool {
	for _, p := range parts {
		if IsModelImage(p) {
			return true
		}
	}
	return false
}

// resolve turns one image artifact reference into a derived inline image
// part, consulting the content-hash cache before touching the store.
func resolve(ctx context.Context, store domain.ArtifactStore, ref domain.ArtifactRef) domain.ContentPart {
	key := ref.ID.String()
	img, ok := deriveCache.get(key)
	if !ok {
		raw, err := store.Read(ctx, ref)
		if err != nil {
			return imagePlaceholder(ref, fmt.Sprintf("read failed: %v", err))
		}
		img, err = Derive(raw)
		if err != nil {
			return imagePlaceholder(ref, err.Error())
		}
		deriveCache.put(key, img)
	}
	image := img // copy; the cache entry is shared
	return domain.ContentPart{Kind: domain.PartImage, Image: &image}
}

// imagePlaceholder degrades an unresolvable image reference to text so the
// model sees a gap it can reason about instead of a failed request.
func imagePlaceholder(ref domain.ArtifactRef, why string) domain.ContentPart {
	return domain.ContentPart{
		Kind: domain.PartText,
		Text: fmt.Sprintf("[image %s unavailable: %s]", ref.ID.String(), why),
	}
}
