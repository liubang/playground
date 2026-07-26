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
// Created: 2026/07/26

package ui

import (
	"strings"
	"testing"
)

func testModelOptions() []ModelOption {
	return []ModelOption{
		{Provider: "aigc", Name: "glm-5.2", ContextWindow: 200000, WireAPI: "responses"},
		{Provider: "aigc", Name: "deepseek-v4-pro", ContextWindow: 1024000, WireAPI: "responses"},
		{Provider: "openai", Name: "gpt-5", ContextWindow: 400000, WireAPI: "chat"},
	}
}

func TestModelPickerCursorStartsAtCurrent(t *testing.T) {
	p := NewModelPicker(testModelOptions(), "aigc/deepseek-v4-pro")
	if p.Cursor != 1 {
		t.Fatalf("cursor = %d, want 1 (the active model)", p.Cursor)
	}
	if got := p.Selected().Ref(); got != "aigc/deepseek-v4-pro" {
		t.Fatalf("selected = %q", got)
	}

	// An unknown current ref leaves the cursor at the top.
	p = NewModelPicker(testModelOptions(), "nosuch/model")
	if p.Cursor != 0 {
		t.Fatalf("cursor = %d, want 0 for unknown current", p.Cursor)
	}
}

func TestModelPickerMoveBounds(t *testing.T) {
	p := NewModelPicker(testModelOptions(), "")
	p.MoveUp()
	if p.Cursor != 0 {
		t.Fatalf("cursor = %d, want clamped 0", p.Cursor)
	}
	p.MoveDown()
	p.MoveDown()
	p.MoveDown()
	p.MoveDown()
	if p.Cursor != 2 {
		t.Fatalf("cursor = %d, want clamped 2", p.Cursor)
	}
}

func TestModelPickerRender(t *testing.T) {
	p := NewModelPicker(testModelOptions(), "aigc/glm-5.2")
	out := p.Render(100, 0)

	for _, want := range []string{
		"Select a model:",
		"▶ aigc/glm-5.2",
		"aigc/deepseek-v4-pro",
		"openai/gpt-5",
		"200k ctx · responses",
		"1.0M ctx · responses",
		"400k ctx · chat",
		"●",
		"j/k or ↑/↓ = move",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("render missing %q:\n%s", want, out)
		}
	}
	// The active marker belongs to the current model only.
	if strings.Contains(out, "deepseek-v4-pro 1.0M ctx · responses ●") {
		t.Errorf("marker leaked to a non-current row:\n%s", out)
	}
}

func TestModelPickerRenderWindowed(t *testing.T) {
	options := make([]ModelOption, 0, 20)
	for i := 0; i < 20; i++ {
		options = append(options, ModelOption{Provider: "p", Name: strings.Repeat("m", 1) + string(rune('a'+i))})
	}
	p := NewModelPicker(options, "")
	p.Cursor = 15
	out := p.Render(60, 10)
	if !strings.Contains(out, "↑ more") {
		t.Errorf("windowed render should hint at rows above:\n%s", out)
	}
	if !strings.Contains(out, "↓ more") {
		t.Errorf("windowed render should hint at rows below:\n%s", out)
	}
	if !strings.Contains(out, "▶ p/mp") {
		t.Errorf("cursor row must stay visible:\n%s", out)
	}
}

func TestFormatTokens(t *testing.T) {
	cases := map[int64]string{
		0:       "0",
		512:     "512",
		65536:   "65k",
		200000:  "200k",
		1024000: "1.0M",
		400000:  "400k",
	}
	for in, want := range cases {
		if got := formatTokens(in); got != want {
			t.Errorf("formatTokens(%d) = %q, want %q", in, got, want)
		}
	}
}
