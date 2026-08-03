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

package domain

import (
	"encoding/base64"
	"os"
	"path/filepath"
	"testing"
)

func TestIsImageExtension(t *testing.T) {
	tests := []struct {
		path string
		want bool
	}{
		{"photo.png", true},
		{"photo.jpg", true},
		{"photo.JPEG", true},
		{"photo.gif", true},
		{"photo.webp", true},
		{"document.pdf", false},
		{"script.sh", false},
		{"noext", false},
	}
	for _, tt := range tests {
		if got := IsImageExtension(tt.path); got != tt.want {
			t.Errorf("IsImageExtension(%q) = %v, want %v", tt.path, got, tt.want)
		}
	}
}

func TestLoadImageFromPath(t *testing.T) {
	// Create a small PNG-like file (8 bytes of fake PNG header).
	dir := t.TempDir()
	content := []byte("\x89PNG\r\n\x1a\n")
	imgPath := filepath.Join(dir, "test.png")
	if err := os.WriteFile(imgPath, content, 0644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	img, err := LoadImageFromPath(imgPath, 1024*1024)
	if err != nil {
		t.Fatalf("LoadImageFromPath() error = %v", err)
	}
	if img.MediaType != "image/png" {
		t.Errorf("MediaType = %q, want image/png", img.MediaType)
	}
	decoded, err := base64.StdEncoding.DecodeString(img.Data)
	if err != nil {
		t.Fatalf("base64 decode: %v", err)
	}
	if string(decoded) != string(content) {
		t.Errorf("decoded data mismatch")
	}

	// Unsupported extension.
	_, err = LoadImageFromPath(filepath.Join(dir, "test.bmp"), 1024*1024)
	if err == nil {
		t.Fatal("expected error for unsupported extension")
	}

	// File too large.
	_, err = LoadImageFromPath(imgPath, 4)
	if err == nil {
		t.Fatal("expected error for oversized file")
	}

	// Non-existent file.
	_, err = LoadImageFromPath(filepath.Join(dir, "missing.png"), 1024*1024)
	if err == nil {
		t.Fatal("expected error for missing file")
	}
}

func TestLoadImageFromPathJPEGExtension(t *testing.T) {
	dir := t.TempDir()
	jpgPath := filepath.Join(dir, "photo.jpeg")
	if err := os.WriteFile(jpgPath, []byte("fake jpeg"), 0644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	img, err := LoadImageFromPath(jpgPath, 1024*1024)
	if err != nil {
		t.Fatalf("LoadImageFromPath() error = %v", err)
	}
	if img.MediaType != "image/jpeg" {
		t.Errorf("MediaType = %q, want image/jpeg", img.MediaType)
	}
}
