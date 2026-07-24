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

package ui

import (
	"strings"
	"testing"
	"time"
)

func TestResolveIconsDefaultsToNerd(t *testing.T) {
	for _, preference := range []string{"", "nerd", "NERD", "anything-else"} {
		if got := ResolveIcons(preference); got != NerdIcons() {
			t.Fatalf("ResolveIcons(%q) = %+v, want nerd set", preference, got)
		}
	}
}

func TestResolveIconsPlain(t *testing.T) {
	for _, preference := range []string{"plain", "PLAIN", " plain "} {
		if got := ResolveIcons(preference); got != PlainIcons() {
			t.Fatalf("ResolveIcons(%q) = %+v, want plain set", preference, got)
		}
	}
}

func TestPlainIconsAvoidEmojiPresentationGlyphs(t *testing.T) {
	// U+26A0 (⚠) renders as a color emoji on many terminals and must stay
	// out of the plain set.
	for name, glyph := range map[string]string{
		"Success":   PlainIcons().Success,
		"Error":     PlainIcons().Error,
		"Cancelled": PlainIcons().Cancelled,
		"Pending":   PlainIcons().Pending,
		"Approval":  PlainIcons().Approval,
		"Warning":   PlainIcons().Warning,
	} {
		if glyph == "" {
			t.Fatalf("plain icon %s is empty", name)
		}
		if strings.ContainsRune(glyph, '⚠') {
			t.Fatalf("plain icon %s contains U+26A0 which renders as emoji", name)
		}
	}
}

func TestIconSetDefaultsForZeroValueModel(t *testing.T) {
	var m Model
	if got := m.iconSet(); got != NerdIcons() {
		t.Fatalf("zero-value model icons = %+v, want nerd default", got)
	}
	m.SetIcons(PlainIcons())
	if got := m.iconSet(); got != PlainIcons() {
		t.Fatalf("after SetIcons(plain) = %+v, want plain set", got)
	}
}

func TestToolStatusIconUsesActiveSet(t *testing.T) {
	block := &TranscriptBlock{Kind: BlockKindTool, Title: "run_cmd", Status: "success"}

	m := Model{theme: NoColorTheme()}
	m.SetIcons(PlainIcons())
	if summary := m.renderToolSummary(block); !strings.Contains(summary, "✓ run_cmd") {
		t.Fatalf("plain success summary = %q, want ✓ prefix", summary)
	}

	m.SetIcons(NerdIcons())
	if summary := m.renderToolSummary(block); !strings.Contains(summary, "\uf00c run_cmd") {
		t.Fatalf("nerd success summary = %q, want \\uf00c prefix", summary)
	}

	block.Status = "cancelled"
	m.SetIcons(NerdIcons())
	if summary := m.renderToolSummary(block); !strings.Contains(summary, "\uf05e") {
		t.Fatalf("nerd cancelled summary = %q, want \\uf05e", summary)
	}
}

func TestToolSummaryRunningUsesSpinnerNotStaticIcon(t *testing.T) {
	m := Model{theme: NoColorTheme()}
	m.SetIcons(PlainIcons())
	block := &TranscriptBlock{
		Kind:      BlockKindTool,
		Title:     "run_cmd",
		Status:    "running",
		StartedAt: time.Now(),
	}
	summary := m.renderToolSummary(block)
	if strings.Contains(summary, "○") || strings.Contains(summary, "\uf10c") {
		t.Fatalf("running summary should use spinner, got %q", summary)
	}
}
