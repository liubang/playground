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
// Created: 2026/08/17

package domain

import "testing"

func TestCanonicalHost(t *testing.T) {
	valid := map[string]string{
		"example.com":      "example.com",
		"  EXAMPLE.com. ":  "example.com",
		"api.example.com":  "api.example.com",
		"127.0.0.1":        "127.0.0.1",
		"xn--nxasmq6b.cn":  "xn--nxasmq6b.cn",
		"weather.com.cn":   "weather.com.cn",
	}
	for in, want := range valid {
		got, err := CanonicalHost(in)
		if err != nil {
			t.Fatalf("CanonicalHost(%q) returned error: %v", in, err)
		}
		if got != want {
			t.Fatalf("CanonicalHost(%q) = %q, want %q", in, got, want)
		}
	}
	invalid := []string{
		"", "   ", ".", ".example.com", "example..com",
		"https://example.com", "example.com/path", "example.com:443",
		"user@example.com", "*.example.com",
	}
	for _, in := range invalid {
		if got, err := CanonicalHost(in); err == nil {
			t.Fatalf("CanonicalHost(%q) = %q, want error", in, got)
		}
	}
}

func TestHostFromURL(t *testing.T) {
	valid := map[string]string{
		"https://example.com/a/b?x=1":      "example.com",
		"http://WWW.Example.COM:8080/x":    "www.example.com",
		"https://example.com":              "example.com",
		" https://api.example.com/v1?q=y ": "api.example.com",
	}
	for in, want := range valid {
		got, ok := HostFromURL(in)
		if !ok {
			t.Fatalf("HostFromURL(%q) not ok", in)
		}
		if got != want {
			t.Fatalf("HostFromURL(%q) = %q, want %q", in, got, want)
		}
	}
	invalid := []string{
		"", "ftp://example.com/x", "file:///etc/passwd", "example.com",
		"https://", "://nohost", "javascript:alert(1)",
	}
	for _, in := range invalid {
		if got, ok := HostFromURL(in); ok {
			t.Fatalf("HostFromURL(%q) = %q, want not ok", in, got)
		}
	}
}
