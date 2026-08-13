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

package e2e

import (
	"bytes"
	"context"
	"encoding/base64"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"gopkg.in/yaml.v3"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// TestServeRealModelImageE2E is the acceptance suite for the artifact-based
// image architecture, run against a REAL LLM provider (the user's own
// ~/.loom/config.yaml). It is skipped unless LOOM_E2E_LLM=1.
//
// The suite needs a vision-capable chat model. The gateway catalog declares
// no modalities, so the test patches an isolated config copy to mark the
// probe model "text+image" (LOOM_E2E_VISION_MODEL overrides the default
// aigc-openai/kimi-k3, probed to accept image input and answer correctly).
//
// Acceptance coverage:
//  1. attachment ingress: a base64 image submitted with the prompt is
//     persisted as an artifact and the vision model actually SEES it
//     (answers the color of a solid-red PNG — impossible without pixels);
//  2. the canonical transcript and the durable event log carry artifact
//     references only — no inline image parts, no base64 blobs;
//  3. the artifact bytes round-trip through the on-disk store;
//  4. tool ingress: view_image output reaches the model through the same
//     reference → materialize-at-egress path;
//  5. modality gating: after switching to a text-only model the same
//     history (with image references) completes a turn — the gateway
//     hard-rejects image parts (glm-5.2 answers 1210), so a finished turn
//     proves media.StripImages sanitized the wire;
//  6. text-only models reject NEW image attachments with an actionable
//     error instead of failing deep inside the provider call.
func TestServeRealModelImageE2E(t *testing.T) {
	if os.Getenv("LOOM_E2E_LLM") != "1" {
		t.Skip("set LOOM_E2E_LLM=1 to run the real-model acceptance suite")
	}

	ctx := context.Background()
	configPath := os.Getenv("LOOM_CONFIG")
	if configPath == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			t.Skipf("no home dir: %v", err)
		}
		configPath = filepath.Join(home, ".loom", "config.yaml")
	}
	configRaw, err := os.ReadFile(configPath)
	if err != nil {
		t.Skipf("loom config not found at %s", configPath)
	}

	visionRef := os.Getenv("LOOM_E2E_VISION_MODEL")
	if visionRef == "" {
		visionRef = "aigc-openai/kimi-k3"
	}
	patched := patchConfigModalities(t, configRaw, visionRef)

	// Same isolation trick as TestServeRealModelE2E: the patched config
	// copy in tmp derives tmp as the loom home, so the user's stores are
	// never touched.
	tmp := t.TempDir()
	isolatedConfig := filepath.Join(tmp, "config.yaml")
	if err := os.WriteFile(isolatedConfig, patched, 0o600); err != nil {
		t.Fatalf("write isolated config: %v", err)
	}
	resolved, err := config.Load(isolatedConfig, config.LoadOptions{RequireProviders: true, Logger: slog.Default()}, os.LookupEnv)
	if err != nil {
		t.Skipf("load loom config: %v", err)
	}
	if err := os.MkdirAll(resolved.Storage.SessionsDir(), 0o700); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	workspace := filepath.Join(tmp, "ws")
	if err := os.MkdirAll(workspace, 0o755); err != nil {
		t.Fatalf("mkdir ws: %v", err)
	}

	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	artifactDir := filepath.Join(tmp, "artifacts")
	proc, err := app.NewProcessRuntime(ctx, resolved, app.ProcessRuntimeConfig{
		ArtifactDir: artifactDir,
		Version:     "e2e",
		Logger:      discard,
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	defer proc.Close()
	bootstrap, err := app.NewWorkspaceBootstrap(ctx, proc, app.BootstrapConfig{
		WorkspaceRoot: workspace,
	})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	defer broker.Close()
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{Logger: discard})
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	}()

	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	sessionID := c.SessionID()

	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c, ch: events}
	go collector.run()

	// Switch to the vision model; the patched modalities must be visible.
	modelResult, err := c.SetModel(ctx, visionRef)
	if err != nil {
		t.Fatalf("SetModel(%q): %v", visionRef, err)
	}
	if !modelResult.Meta.SupportsImages() {
		t.Fatalf("SetModel(%q): patched modalities not in effect (SupportsImages=false)", visionRef)
	}

	pngBytes := solidRedPNG(t)
	b64 := base64.StdEncoding.EncodeToString(pngBytes)

	// --- 1. attachment ingress: the model must actually see the image ---
	turns := collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx,
		"这张图片是纯色的。它是什么颜色？只回答一个中文颜色词，不要解释。",
		[]domain.ImageContent{{MediaType: "image/png", Data: b64}}); err != nil {
		t.Fatalf("SubmitPrompt(image turn): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	finalText := lastAssistantText(snap.Messages)
	if !strings.Contains(finalText, "红") {
		dumpImageTurnFailure(t, snap, collector)
		t.Fatalf("vision model answer %q does not name red (materialize-at-egress broken?)", finalText)
	}
	t.Logf("attachment ingress ok: model saw the image, answered %q", strings.TrimSpace(finalText))

	// --- 2. transcript + durable log hold references, never bytes ---
	attachRef := findUserImageArtifact(t, snap.Messages)
	assertNoInlineImages(t, snap.Messages)

	store, err := sessionStoreReadOnly(ctx, resolved)
	if err != nil {
		t.Fatalf("open session store: %v", err)
	}
	persisted, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	sawArtifactRef := false
	for _, evt := range persisted {
		payload := string(evt.Payload)
		if strings.Contains(payload, b64) || strings.Contains(payload, "data:image/") {
			t.Fatalf("base64 blob leaked into durable event %s: %.200s...", evt.Type, payload)
		}
		if strings.Contains(payload, attachRef.ID.String()) {
			sawArtifactRef = true
		}
	}
	store.Close()
	if !sawArtifactRef {
		t.Fatalf("no durable event references the attachment artifact %s", attachRef.ID)
	}
	t.Logf("durable log ok: %d events, references only (no base64)", len(persisted))

	// --- 3. artifact bytes round-trip through the on-disk store ---
	astore, err := artifact.Open(artifactDir, 100<<20)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	raw, err := astore.Read(ctx, attachRef)
	if err != nil {
		t.Fatalf("artifact read: %v", err)
	}
	if !bytes.Equal(raw, pngBytes) {
		t.Fatalf("artifact bytes mismatch: got %d bytes, want %d", len(raw), len(pngBytes))
	}
	t.Logf("artifact round-trip ok: %s (%d bytes, %s)", attachRef.ID, attachRef.Size, attachRef.MediaType)

	// --- 4. tool ingress: view_image output reaches the model ---
	swatch := filepath.Join(workspace, "swatch.png")
	if err := os.WriteFile(swatch, pngBytes, 0o600); err != nil {
		t.Fatalf("write swatch: %v", err)
	}
	turns = collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, fmt.Sprintf(
		"调用 view_image 工具查看图片 %s，然后只回答一个中文颜色词，不要解释。", swatch,
	), nil); err != nil {
		t.Fatalf("SubmitPrompt(view_image turn): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)

	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(after view_image): %v", err)
	}
	finalText = lastAssistantText(snap.Messages)
	if !strings.Contains(finalText, "红") {
		dumpImageTurnFailure(t, snap, collector)
		t.Fatalf("view_image answer %q does not name red (tool-ingress materialize broken?)", finalText)
	}
	assertToolResultImageArtifact(t, snap.Messages)
	assertNoInlineImages(t, snap.Messages)
	t.Logf("tool ingress ok: view_image artifact materialized, answered %q", strings.TrimSpace(finalText))

	// --- 5. modality gating: a text-only model gets gaps, not 1210s ---
	if _, err := c.SetModel(ctx, "aigc-openai/glm-5.2"); err != nil {
		t.Fatalf("SetModel(glm-5.2): %v", err)
	}
	turns = collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx,
		"直接回答：你现在能看到我们对话里的图片吗？一句话。", nil); err != nil {
		t.Fatalf("SubmitPrompt(gated turn): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)
	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(after gating): %v", err)
	}
	if snap.State != app.ControllerStateIdle || lastAssistantText(snap.Messages) == "" {
		dumpImageTurnFailure(t, snap, collector)
		t.Fatalf("gated turn did not complete cleanly (state=%s) — image parts must have leaked onto the wire", snap.State)
	}
	t.Logf("modality gating ok: text-only model completed a turn over image history, answered %q",
		strings.TrimSpace(lastAssistantText(snap.Messages)))

	// --- 6. text-only models reject new attachments up front ---
	if _, err := c.SubmitPrompt(ctx, "带图消息",
		[]domain.ImageContent{{MediaType: "image/png", Data: b64}}); err == nil {
		t.Fatal("image submission to a text-only model must be rejected")
	} else if !strings.Contains(err.Error(), "does not support image input") {
		t.Fatalf("rejection error = %v, want an actionable modalities message", err)
	}
	t.Log("attachment rejection ok")

	t.Log("ACCEPTANCE PASS: artifact-based image architecture verified against a real provider")
}

// patchConfigModalities returns the config with modalities ["text","image"]
// injected into the model named by ref ("provider/model", or a bare model
// name matched across providers). Everything else is preserved verbatim.
func patchConfigModalities(t *testing.T, raw []byte, ref string) []byte {
	t.Helper()
	provider, model, hasSlash := strings.Cut(ref, "/")
	if !hasSlash {
		provider, model = "", provider // bare form: the whole ref is the model name
	}

	var doc map[string]any
	if err := yaml.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("parse loom config: %v", err)
	}
	providers, _ := doc["providers"].([]any)
	patched := false
	for _, p := range providers {
		pm, _ := p.(map[string]any)
		if provider != "" && pm["name"] != provider {
			continue
		}
		models, _ := pm["models"].([]any)
		for _, m := range models {
			mm, _ := m.(map[string]any)
			if mm["name"] == model {
				mm["modalities"] = []any{"text", "image"}
				patched = true
			}
		}
	}
	if !patched {
		t.Fatalf("vision model %q not found in the config catalog", ref)
	}
	out, err := yaml.Marshal(doc)
	if err != nil {
		t.Fatalf("re-marshal config: %v", err)
	}
	return out
}

// solidRedPNG renders a 128x128 pure-red PNG: a vision question with one
// unambiguous answer, cheap to send and to verify.
func solidRedPNG(t *testing.T) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, 128, 128))
	draw.Draw(img, img.Bounds(), &image.Uniform{color.RGBA{R: 0xFF, A: 0xFF}}, image.Point{}, draw.Src)
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("png.Encode: %v", err)
	}
	return buf.Bytes()
}

// findUserImageArtifact locates the image artifact reference on the user
// attachment message, failing the test when the transcript lost it.
func findUserImageArtifact(t *testing.T, messages []domain.Message) domain.ArtifactRef {
	t.Helper()
	for _, msg := range messages {
		if msg.Role != domain.RoleUser {
			continue
		}
		for _, part := range msg.Parts {
			if part.Kind == domain.PartArtifact && part.Artifact != nil &&
				strings.HasPrefix(part.Artifact.MediaType, "image/") {
				return *part.Artifact
			}
		}
	}
	t.Fatal("no image artifact reference on any user message")
	return domain.ArtifactRef{}
}

// assertNoInlineImages fails when any transcript message carries an inline
// base64 image part — the canonical history must hold references only.
func assertNoInlineImages(t *testing.T, messages []domain.Message) {
	t.Helper()
	for i, msg := range messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartImage {
				t.Fatalf("message %d carries an inline image part (base64 in the transcript)", i)
			}
			if part.ToolResult != nil {
				for _, cp := range part.ToolResult.Content {
					if cp.Kind == domain.PartImage {
						t.Fatalf("message %d tool result carries an inline image part", i)
					}
				}
			}
		}
	}
}

// assertToolResultImageArtifact fails when no tool result carries an image
// artifact reference (view_image must persist its output as an artifact).
func assertToolResultImageArtifact(t *testing.T, messages []domain.Message) {
	t.Helper()
	for _, msg := range messages {
		for _, part := range msg.Parts {
			if part.ToolResult == nil {
				continue
			}
			for _, cp := range part.ToolResult.Content {
				if cp.Kind == domain.PartArtifact && cp.Artifact != nil &&
					strings.HasPrefix(cp.Artifact.MediaType, "image/") {
					return
				}
			}
		}
	}
	t.Fatal("no image artifact reference in any tool result (view_image output missing)")
}

// dumpImageTurnFailure prints the diagnostics that matter when a vision
// turn comes back empty: the controller state, the turn error observed on
// the event stream, and a shape summary of every transcript message.
func dumpImageTurnFailure(t *testing.T, snap client.Snapshot, collector *eventCollector) {
	t.Helper()
	t.Logf("state=%s turns=%d lastTurnError=%q", snap.State, snap.TurnCount, collector.lastTurnError())
	for i, msg := range snap.Messages {
		kinds := make([]string, 0, len(msg.Parts))
		for _, part := range msg.Parts {
			kind := string(part.Kind)
			if part.Kind == domain.PartText && part.Text != "" {
				text := part.Text
				if len(text) > 120 {
					text = text[:120] + "..."
				}
				kind += "(" + text + ")"
			}
			if part.ToolResult != nil && part.ToolResult.Error != nil {
				kind += "[tool-error: " + part.ToolResult.Error.Message + "]"
			}
			kinds = append(kinds, kind)
		}
		t.Logf("  msg[%d] role=%s parts=%v", i, msg.Role, kinds)
	}
}
