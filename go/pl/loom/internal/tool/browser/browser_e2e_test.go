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

package browser

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// e2ePage is the HTML fixture for the end-to-end test. It exercises every
// interactive action: a JS-mutating button (click), a GET form with a text
// input (type + submit via Enter), and enough vertical content to scroll.
const e2ePage = `<!DOCTYPE html>
<html>
<head><title>Loom Browser E2E</title></head>
<body>
<h1>Browser E2E Fixture</h1>
<button id="counter" onclick="window.c=(window.c||0)+1;this.textContent='Count: '+window.c">Count: 0</button>
<form action="/search" method="get">
  <label for="q">Query</label>
  <input type="text" id="q" name="q">
  <button type="submit">Search</button>
</form>
<div style="height:3000px">tall content for scrolling</div>
<footer>bottom of page</footer>
</body>
</html>`

// e2eHandler serves the fixture page and the form-submission landing page.
func e2eHandler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = w.Write([]byte(e2ePage))
	})
	mux.HandleFunc("/search", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = fmt.Fprintf(w, `<!DOCTYPE html><html><head><title>Search Results</title></head>`+
			`<body><p>results for: %s</p></body></html>`, r.URL.Query().Get("q"))
	})
	return mux
}

// e2eEnv bundles the tool under test with helpers that drive it through the
// full Prepare → Execute pipeline, exactly as the agent runtime would.
type e2eEnv struct {
	t    *testing.T
	tool *BrowserTool
}

func newE2EEnv(t *testing.T) *e2eEnv {
	t.Helper()
	chromePath := findChrome()
	if chromePath == "" {
		t.Skip("Chrome/Chromium binary not found; skipping browser e2e test")
	}
	mgr, err := NewManager(chromePath, "", time.Minute, 1280, 720)
	require.NoError(t, err)
	t.Cleanup(mgr.Close)

	tool, err := NewBrowserTool(mgr, &mockArtifactStore{}, 15*time.Second, 0)
	require.NoError(t, err)
	return &e2eEnv{t: t, tool: tool}
}

// findPart returns the first content part of the given kind, or nil.
func findPart(content []domain.ContentPart, kind domain.PartKind) *domain.ContentPart {
	for i := range content {
		if content[i].Kind == kind {
			return &content[i]
		}
	}
	return nil
}

// exec runs one tool call end to end and fails the test on any error.
func (e *e2eEnv) exec(args browserArgs) browserOutput {
	e.t.Helper()
	out, result := e.execRaw(args)
	require.Equal(e.t, domain.ToolStatusSuccess, result.Status,
		"browser %s failed: %+v", args.Action, result.Error)
	return out
}

// execRaw runs one tool call without asserting success, for negative tests.
// The text output is parsed from the first text part; other parts (image,
// artifact) remain accessible on the returned ToolResult.
func (e *e2eEnv) execRaw(args browserArgs) (browserOutput, domain.ToolResult) {
	e.t.Helper()
	call := newToolCall(e.t, "browser", args)
	prepared, err := e.tool.Prepare(context.Background(), call)
	require.NoError(e.t, err)
	result := e.tool.Execute(context.Background(), prepared)
	var out browserOutput
	if result.Status == domain.ToolStatusSuccess {
		text := findPart(result.Content, domain.PartText)
		require.NotNil(e.t, text, "successful result must carry a text header part")
		require.NoError(e.t, json.Unmarshal([]byte(text.Text), &out))
	}
	return out, result
}

// findRef returns the registry ref for the first node with the given role,
// verifying that the ref also appears in the serialized snapshot output.
func (e *e2eEnv) findRef(snapshotOutput, role string) string {
	e.t.Helper()
	e.tool.registry.mu.Lock()
	defer e.tool.registry.mu.Unlock()
	for ref, n := range e.tool.registry.refs {
		if n.role == role {
			require.Contains(e.t, snapshotOutput, ref+" "+role,
				"registry ref %q must appear in the serialized snapshot", ref)
			return ref
		}
	}
	return ""
}

// waitForSnapshot polls the snapshot action until its output satisfies pred.
func (e *e2eEnv) waitForSnapshot(pred func(string) bool, what string) string {
	e.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	var out string
	for time.Now().Before(deadline) {
		out = e.exec(browserArgs{Action: "snapshot"}).Output
		if pred(out) {
			return out
		}
		time.Sleep(100 * time.Millisecond)
	}
	e.t.Fatalf("timed out waiting for %s; last snapshot:\n%s", what, out)
	return ""
}

// TestBrowserTool_E2E_FullFlow drives a real headless Chrome through the
// complete action pipeline against a local HTTP server.
func TestBrowserTool_E2E_FullFlow(t *testing.T) {
	env := newE2EEnv(t)
	srv := httptest.NewServer(e2eHandler())
	defer srv.Close()

	// 1. navigate: load the fixture page.
	nav := env.exec(browserArgs{Action: "navigate", URL: srv.URL})
	assert.Equal(t, "navigate", nav.Action)
	assert.Equal(t, "Loom Browser E2E", nav.Title)
	assert.Equal(t, "ok", nav.Status)

	// 2. snapshot: the AX tree must list the interactive elements with refs.
	// Chrome builds the AX tree lazily after Accessibility.enable, so poll
	// until the tree is populated.
	snapText := env.waitForSnapshot(func(s string) bool {
		return strings.Contains(s, "Count: 0")
	}, "AX tree to populate after navigate")
	assert.Contains(t, snapText, "Accessibility Tree Snapshot")
	counterRef := env.findRef(snapText, "button")
	require.NotEmpty(t, counterRef, "snapshot must assign a ref to the counter button")

	// 3. click: the counter button mutates its own label via JS.
	click := env.exec(browserArgs{Action: "click", Ref: counterRef})
	assert.Equal(t, "click", click.Action)
	assert.Equal(t, counterRef, click.Ref)
	assert.Equal(t, "ok", click.Status)

	// Click invalidates the registry: the ref must now be rejected.
	_, staleResult := env.execRaw(browserArgs{Action: "click", Ref: counterRef})
	assert.Equal(t, domain.ToolStatusError, staleResult.Status)
	require.NotNil(t, staleResult.Error)
	assert.Contains(t, staleResult.Error.Message, "no live snapshot refs")

	// The label mutation must be visible in a fresh snapshot.
	snapText = env.waitForSnapshot(func(s string) bool {
		return strings.Contains(s, "Count: 1")
	}, "counter label to update after click")

	// 4. type + submit: fill the search box and press Enter to submit the
	// form, which navigates to /search?q=...
	textboxRef := env.findRef(snapText, "textbox")
	require.NotEmpty(t, textboxRef, "snapshot must assign a ref to the search input")
	typed := env.exec(browserArgs{Action: "type", Ref: textboxRef, Text: "loom-e2e", Submit: true})
	assert.Equal(t, "type", typed.Action)
	assert.Equal(t, "ok", typed.Status)

	// The form submission navigates away from the fixture page.
	env.waitForSnapshot(func(s string) bool {
		return strings.Contains(s, "results for: loom-e2e")
	}, "form submission to land on the search results page")

	// 5. navigate back, then scroll the tall fixture page.
	env.exec(browserArgs{Action: "navigate", URL: srv.URL})
	scrolled := env.exec(browserArgs{Action: "scroll", ScrollY: 1000})
	require.NotNil(t, scrolled.ScrollPos)
	assert.Greater(t, scrolled.ScrollPos.Y, 500, "scroll must move the viewport")

	// 6. screenshot: the header carries metadata, the image goes out as an
	// artifact (for the user) plus an inline image part (for the model).
	// The base64 payload must NOT appear in the text header.
	pngShot, pngResult := env.execRaw(browserArgs{Action: "screenshot"})
	require.Equal(t, domain.ToolStatusSuccess, pngResult.Status, "png screenshot: %+v", pngResult.Error)
	require.NotNil(t, pngShot.Screenshot)
	assert.Equal(t, "png", pngShot.Screenshot.Format)
	assert.True(t, pngShot.Screenshot.Inlined)
	assert.NotEmpty(t, pngShot.Screenshot.Note)
	assert.NotContains(t, findPart(pngResult.Content, domain.PartText).Text, "iVBORw", "base64 must not leak into the text header")
	pngArt := findPart(pngResult.Content, domain.PartArtifact)
	require.NotNil(t, pngArt, "screenshot must be persisted as an artifact")
	assert.Equal(t, "image/png", pngArt.Artifact.MediaType)
	pngImg := findPart(pngResult.Content, domain.PartImage)
	require.NotNil(t, pngImg, "screenshot must be inlined as an image part")
	assert.Equal(t, "image/png", pngImg.Image.MediaType)
	pngData, err := base64.StdEncoding.DecodeString(pngImg.Image.Data)
	require.NoError(t, err)
	assert.True(t, strings.HasPrefix(string(pngData), "\x89PNG"), "PNG magic bytes")

	jpgShot, jpgResult := env.execRaw(browserArgs{Action: "screenshot", Format: "jpeg", Quality: 50, FullPage: true})
	require.Equal(t, domain.ToolStatusSuccess, jpgResult.Status, "jpeg screenshot: %+v", jpgResult.Error)
	require.NotNil(t, jpgShot.Screenshot)
	assert.Equal(t, "jpeg", jpgShot.Screenshot.Format)
	jpgImg := findPart(jpgResult.Content, domain.PartImage)
	require.NotNil(t, jpgImg)
	assert.Equal(t, "image/jpeg", jpgImg.Image.MediaType)
	jpgData, err := base64.StdEncoding.DecodeString(jpgImg.Image.Data)
	require.NoError(t, err)
	assert.True(t, len(jpgData) > 2 && jpgData[0] == 0xFF && jpgData[1] == 0xD8, "JPEG magic bytes")

	// 7. close: releases the instance; the next navigate must transparently
	// create a fresh one.
	closed := env.exec(browserArgs{Action: "close"})
	assert.Equal(t, "close", closed.Action)
	assert.Equal(t, "ok", closed.Status)

	reopened := env.exec(browserArgs{Action: "navigate", URL: srv.URL})
	assert.Equal(t, "ok", reopened.Status)
	assert.Equal(t, "Loom Browser E2E", reopened.Title)
}

// TestBrowserTool_E2E_StaleRefAfterNavigate verifies the navigate →
// invalidate → reject cycle against a live browser.
func TestBrowserTool_E2E_StaleRefAfterNavigate(t *testing.T) {
	env := newE2EEnv(t)
	srv := httptest.NewServer(e2eHandler())
	defer srv.Close()

	env.exec(browserArgs{Action: "navigate", URL: srv.URL})
	snapText := env.waitForSnapshot(func(s string) bool {
		return strings.Contains(s, "Count: 0")
	}, "AX tree to populate after navigate")
	ref := env.findRef(snapText, "button")
	require.NotEmpty(t, ref)

	// Navigating away invalidates refs captured on the previous page.
	env.exec(browserArgs{Action: "navigate", URL: srv.URL + "/search?q=other"})
	_, result := env.execRaw(browserArgs{Action: "click", Ref: ref})
	assert.Equal(t, domain.ToolStatusError, result.Status)
	require.NotNil(t, result.Error)
	assert.Contains(t, result.Error.Message, "no live snapshot refs")
}

// TestBrowserTool_E2E_TypeUnicode verifies the IME text-insertion path
// handles non-ASCII text (chromedp.SendKeys cannot).
func TestBrowserTool_E2E_TypeUnicode(t *testing.T) {
	env := newE2EEnv(t)
	srv := httptest.NewServer(e2eHandler())
	defer srv.Close()

	env.exec(browserArgs{Action: "navigate", URL: srv.URL})
	snapText := env.waitForSnapshot(func(s string) bool {
		return strings.Contains(s, "Count: 0")
	}, "AX tree to populate after navigate")
	ref := env.findRef(snapText, "textbox")
	require.NotEmpty(t, ref)

	env.exec(browserArgs{Action: "type", Ref: ref, Text: "你好 loom", Submit: true})
	env.waitForSnapshot(func(s string) bool {
		return strings.Contains(s, "results for: 你好 loom")
	}, "unicode form submission")
}
