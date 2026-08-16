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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// pickerTestHost renders finders without colors so assertions match raw
// text.
func pickerTestHost() Model {
	return Model{theme: NoColorTheme()}
}

func TestReasoningFinderCursorOnActiveDial(t *testing.T) {
	host := pickerTestHost()

	// Following the model config: cursor and badge on "default".
	f := host.NewReasoningFinder("", false)
	if got := f.Selected().Arg; got != "default" {
		t.Fatalf("cursor = %q, want default", got)
	}
	out := f.Render(80, 0)
	if !strings.Contains(out, "default") || !strings.Contains(out, "●") {
		t.Fatalf("render missing active marker:\n%s", out)
	}

	// Session override active: cursor and badge on the override level.
	f = host.NewReasoningFinder("high", true)
	if got := f.Selected().Arg; got != "high" {
		t.Fatalf("cursor = %q, want high", got)
	}
}

func TestReasoningFinderNavigationBounds(t *testing.T) {
	host := pickerTestHost()
	f := host.NewReasoningFinder("", false)
	f.MoveUp() // already at the top: stays
	if got := f.Selected().Arg; got != "default" {
		t.Fatalf("selected = %q, want default (clamped)", got)
	}
	for i := 0; i < len(ReasoningLevels)+2; i++ {
		f.MoveDown()
	}
	if got := f.Selected().Arg; got != "high" {
		t.Fatalf("selected = %q, want high (clamped)", got)
	}
}

func testModelOptions() []ModelOption {
	return []ModelOption{
		{Provider: "aigc", Name: "glm-5.2", ContextWindow: 200000, WireAPI: "responses"},
		{Provider: "aigc", Name: "deepseek-v4-pro", ContextWindow: 1024000, WireAPI: "responses"},
		{Provider: "openai", Name: "gpt-5", ContextWindow: 400000, WireAPI: "chat"},
	}
}

func TestModelFinderCursorStartsAtCurrent(t *testing.T) {
	host := pickerTestHost()
	f := host.NewModelFinder(testModelOptions(), "aigc/deepseek-v4-pro")
	if got := f.Selected().Ref(); got != "aigc/deepseek-v4-pro" {
		t.Fatalf("selected = %q, want the active model", got)
	}

	// An unknown current ref leaves the cursor at the top.
	f = host.NewModelFinder(testModelOptions(), "nosuch/model")
	if got := f.Selected().Ref(); got != "aigc/glm-5.2" {
		t.Fatalf("selected = %q, want the first option for unknown current", got)
	}
}

func TestModelFinderRender(t *testing.T) {
	host := pickerTestHost()
	f := host.NewModelFinder(testModelOptions(), "aigc/glm-5.2")
	out := f.Render(100, 0)

	for _, want := range []string{
		"❯", // the filter input line
		"3/3",
		"▶ aigc/glm-5.2",
		"aigc/deepseek-v4-pro",
		"openai/gpt-5",
		"200k ctx · responses",
		"1.0M ctx · responses",
		"400k ctx · chat",
		"INSERT",
		"Wire API: responses", // the preview pane
	} {
		if !strings.Contains(out, want) {
			t.Errorf("render missing %q:\n%s", want, out)
		}
	}
	// The active badge belongs to the current model only.
	if strings.Count(out, "●") != 1 {
		t.Errorf("badge should mark exactly one row:\n%s", out)
	}
}

func TestModelFinderRenderWindowed(t *testing.T) {
	host := pickerTestHost()
	options := make([]ModelOption, 0, 20)
	for i := 0; i < 20; i++ {
		options = append(options, ModelOption{Provider: "p", Name: "m" + string(rune('a'+i))})
	}
	f := host.NewModelFinder(options, "")
	for i := 0; i < 15; i++ {
		f.MoveDown()
	}
	out := f.Render(60, 10)
	if !strings.Contains(out, "↑ more") {
		t.Errorf("windowed render should hint at rows above:\n%s", out)
	}
	if !strings.Contains(out, "↓ more") {
		t.Errorf("windowed render should hint at rows below:\n%s", out)
	}
	if !strings.Contains(out, "▶ p/mp") {
		t.Errorf("cursor row must stay visible:\n%s", out)
	}
	// The frame must never exceed the height budget (inline renderer
	// line tracking depends on it).
	if lines := strings.Count(out, "\n") + 1; lines > 10 {
		t.Errorf("render used %d lines, budget 10:\n%s", lines, out)
	}
}

func TestModelFinderFilter(t *testing.T) {
	host := pickerTestHost()
	f := host.NewModelFinder(testModelOptions(), "")
	for _, r := range "gpt" {
		f.TypeRune(r)
	}
	if f.Len() != 1 {
		t.Fatalf("filtered len = %d, want 1", f.Len())
	}
	if got := f.Selected().Ref(); got != "openai/gpt-5" {
		t.Fatalf("selected = %q, want openai/gpt-5", got)
	}
	f.Backspace()
	f.Backspace()
	f.Backspace()
	if f.Len() != 3 {
		t.Fatalf("len after clearing the query = %d, want 3", f.Len())
	}
}

func TestSessionFinderItemsAndPreview(t *testing.T) {
	host := pickerTestHost()
	f := host.NewSessionFinder()
	if out := f.Render(80, 0); !strings.Contains(out, "Loading") {
		t.Fatalf("unloaded finder should render the loading state:\n%s", out)
	}
	summaries := []app.SessionSummary{{
		ID:        domain.NewSessionID(),
		Version:   3,
		CreatedAt: time.Now(),
		UpdatedAt: time.Now(),
	}}
	f.Load(sessionFinderItems(summaries), nil)
	out := f.Render(100, 0)
	if !strings.Contains(out, "v3 ·") {
		t.Errorf("row hint missing version:\n%s", out)
	}
	if !strings.Contains(out, "Version:  3") {
		t.Errorf("preview pane missing metadata:\n%s", out)
	}
}

// A session blocked on a human must stand out in the /sessions picker
// (mirroring the WebUI sidebar's amber dot): awaiting_approval gets a
// badge plus a loud hint, running a quiet hint prefix, idle stays silent.
func TestSessionFinderItemsSurfaceLiveState(t *testing.T) {
	summaries := []app.SessionSummary{
		{ID: domain.NewSessionID(), Version: 1, UpdatedAt: time.Now(), State: app.ControllerStateAwaitingApproval},
		{ID: domain.NewSessionID(), Version: 2, UpdatedAt: time.Now(), State: app.ControllerStateRunning},
		{ID: domain.NewSessionID(), Version: 3, UpdatedAt: time.Now(), State: app.ControllerStateIdle},
	}
	items := sessionFinderItems(summaries)
	if items[0].Badge == "" {
		t.Errorf("awaiting_approval session missing badge: %+v", items[0])
	}
	if !strings.Contains(items[0].Hint, "awaiting approval") {
		t.Errorf("awaiting_approval session missing hint: %+v", items[0])
	}
	if !strings.Contains(items[1].Hint, "running") {
		t.Errorf("running session missing hint: %+v", items[1])
	}
	if items[2].Badge != "" || strings.Contains(items[2].Hint, "awaiting") || strings.Contains(items[2].Hint, "running") {
		t.Errorf("idle session should stay quiet: %+v", items[2])
	}
	// The fuzzy-filter text stays the bare session ID for every state.
	for _, item := range items {
		if item.Text != item.Value.ID.String() {
			t.Errorf("filter text = %q, want %q", item.Text, item.Value.ID.String())
		}
	}
}

func TestSessionPreviewIncludesState(t *testing.T) {
	live := sessionPreview(app.SessionSummary{State: app.ControllerStateAwaitingApproval})
	if !strings.Contains(live, "State:    awaiting_approval") {
		t.Errorf("preview missing live state:\n%s", live)
	}
	detached := sessionPreview(app.SessionSummary{})
	if !strings.Contains(detached, "State:    closed") {
		t.Errorf("preview should report detached sessions as closed:\n%s", detached)
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
