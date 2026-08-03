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
// Created: 2026/08/03

package domain

import (
	"encoding/base64"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// imageExtensions maps file extensions to their MIME media types.
var imageExtensions = map[string]string{
	".png":  "image/png",
	".jpg":  "image/jpeg",
	".jpeg": "image/jpeg",
	".gif":  "image/gif",
	".webp": "image/webp",
}

// IsImageExtension reports whether the file extension is a supported image
// format (png, jpg, jpeg, gif, webp).
func IsImageExtension(path string) bool {
	ext := strings.ToLower(filepath.Ext(path))
	_, ok := imageExtensions[ext]
	return ok
}

// LoadImageFromPath reads an image file, determines its media type from the
// extension, and returns an ImageContent with the file data base64-encoded.
// Returns an error if the file cannot be read, the extension is not a
// supported image type, or the file exceeds maxBytes.
func LoadImageFromPath(path string, maxBytes int64) (ImageContent, error) {
	ext := strings.ToLower(filepath.Ext(path))
	mediaType, ok := imageExtensions[ext]
	if !ok {
		return ImageContent{}, fmt.Errorf("unsupported image extension %q (supported: .png, .jpg, .jpeg, .gif, .webp)", ext)
	}

	stat, err := os.Stat(path)
	if err != nil {
		return ImageContent{}, fmt.Errorf("stat image %q: %w", path, err)
	}
	if stat.Size() > maxBytes {
		return ImageContent{}, fmt.Errorf("image %q is %d bytes, exceeds limit of %d bytes", path, stat.Size(), maxBytes)
	}

	raw, err := os.ReadFile(path)
	if err != nil {
		return ImageContent{}, fmt.Errorf("read image %q: %w", path, err)
	}

	return ImageContent{
		MediaType: mediaType,
		Data:      base64.StdEncoding.EncodeToString(raw),
	}, nil
}
