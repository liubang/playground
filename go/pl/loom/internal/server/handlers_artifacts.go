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
// Created: 2026/08/06

package server

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// handleArtifact serves a committed artifact's raw bytes by content-derived
// ID. The size query parameter is required: the artifact store verifies the
// content hash against the full reference (ID + size) to detect tampering.
// The endpoint is used by the web frontend to render images stored as
// artifacts (e.g. generate_image tool output) without embedding multi-MB
// base64 blobs into the transcript JSON.
func (s *Server) handleArtifact(w http.ResponseWriter, r *http.Request) {
	ref, err := parseArtifactRefParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	data, err := s.svc.ReadArtifact(r.Context(), ref)
	if err != nil {
		writeError(w, err)
		return
	}
	serveArtifactBytes(w, data)
}

// parseArtifactRefParam validates the {id} path value and the required
// size query parameter into an ArtifactRef (REVIEW R14: shared by the
// owner and share-link artifact endpoints).
func parseArtifactRefParam(r *http.Request) (domain.ArtifactRef, error) {
	rawID := r.PathValue("id")
	id, err := domain.ParseArtifactID(rawID)
	if err != nil || !domain.HasPrefix(id, "art_") {
		return domain.ArtifactRef{}, invalidInput("invalid artifact id")
	}
	sizeStr := r.URL.Query().Get("size")
	if sizeStr == "" {
		return domain.ArtifactRef{}, invalidInput("size query parameter is required")
	}
	size, err := strconv.ParseInt(sizeStr, 10, 64)
	if err != nil || size < 0 {
		return domain.ArtifactRef{}, invalidInput("size must be a non-negative integer")
	}
	return domain.ArtifactRef{ID: id, Size: size}, nil
}

// serveArtifactBytes writes an artifact blob with content-type sniffing
// and immutable caching: the ID is content-derived, so the bytes behind a
// given URL never change (REVIEW R14).
func serveArtifactBytes(w http.ResponseWriter, data []byte) {
	w.Header().Set("Content-Type", detectArtifactContentType(data))
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

// detectArtifactContentType sniffs the media type of artifact bytes. The
// artifact store is content-addressed by SHA-256, so the type is not stored
// alongside the blob; sniffing at read time is the simplest approach for the
// common case (images). Falls back to application/octet-stream.
func detectArtifactContentType(data []byte) string {
	// http.DetectContentType recognizes common image formats (png, jpeg, gif,
	// webp) and many others. We trim the "; charset=..." suffix that
	// DetectContentType appends for text types — irrelevant for binary blobs.
	ct := http.DetectContentType(data)
	if i := strings.IndexByte(ct, ';'); i >= 0 {
		ct = strings.TrimSpace(ct[:i])
	}
	return ct
}
