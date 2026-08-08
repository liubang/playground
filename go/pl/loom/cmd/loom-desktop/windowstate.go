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

// Window geometry persistence: macOS users expect an app's window to
// reappear where they left it. State lives in a small JSON file under the
// loom data dir (alongside sessions/logs), written on shutdown and applied
// on startup.

package main

import (
	"context"
	"encoding/json"
	"log/slog"
	"os"
	"path/filepath"
	"time"

	wailsrt "github.com/wailsapp/wails/v2/pkg/runtime"

	"github.com/liubang/playground/go/pl/loom/internal/config"
)

const (
	defaultWindowWidth  = 1280
	defaultWindowHeight = 860
	minWindowWidth      = 960
	minWindowHeight     = 640
)

// windowState is the persisted window geometry.
type windowState struct {
	Width  int `json:"width"`
	Height int `json:"height"`
	X      int `json:"x"`
	Y      int `json:"y"`
}

func windowStatePath(resolved *config.ResolvedConfig) string {
	return filepath.Join(resolved.Storage.BaseDir, "desktop-window.json")
}

// loadWindowState reads the saved geometry; ok is false when no usable
// state exists (first launch, corrupt file, implausible values).
func loadWindowState(path string) (ws windowState, ok bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return windowState{}, false
	}
	if err := json.Unmarshal(data, &ws); err != nil {
		return windowState{}, false
	}
	if ws.Width < minWindowWidth || ws.Height < minWindowHeight {
		return windowState{}, false
	}
	return ws, true
}

// restoreWindowPosition moves the window to the saved origin when it still
// lands on an attached display. A monitor unplugged between runs leaves a
// stale origin; in that case the window keeps the OS default placement
// rather than materializing off-screen.
func restoreWindowPosition(ctx context.Context, ws windowState) {
	screens, err := wailsrt.ScreenGetAll(ctx)
	if err != nil || len(screens) == 0 {
		return
	}
	// The window must be at least partially reachable: its origin within a
	// screen, with 100px of slack so menu-bar-height overlaps still count.
	for _, sc := range screens {
		if ws.X >= -ws.Width+100 && ws.Y >= 0 &&
			ws.X <= sc.Size.Width-100 && ws.Y <= sc.Size.Height-100 {
			wailsrt.WindowSetPosition(ctx, ws.X, ws.Y)
			return
		}
	}
}

// persistWindowState polls the window geometry while the app runs and
// rewrites the state file whenever it changes.
//
// Polling is deliberate: wails runs OnShutdown AFTER WindowClose() has
// released the native window context (internal/app Run: frontend.Run →
// RunMainLoop → WindowClose → shutdownCallback), so querying geometry from
// OnShutdown dereferences a freed WailsContext. A crash mid-session also
// loses nothing, since every change is persisted immediately.
//
// ctx is the app-lifetime (signal) context; wctx is the wails runtime
// context carrying the window handles.
func persistWindowState(ctx context.Context, wctx context.Context, path string, logger *slog.Logger) {
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()
	var last windowState
	for {
		if ws, ok := currentWindowState(wctx); ok && ws != last {
			writeWindowState(path, ws, logger)
			last = ws
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

// currentWindowState reads the live geometry. Degenerate sizes (minimized
// or fullscreen transitions) are rejected so they never overwrite the last
// good state.
func currentWindowState(wctx context.Context) (windowState, bool) {
	w, h := wailsrt.WindowGetSize(wctx)
	x, y := wailsrt.WindowGetPosition(wctx)
	if w < minWindowWidth || h < minWindowHeight {
		return windowState{}, false
	}
	return windowState{Width: w, Height: h, X: x, Y: y}, true
}

func writeWindowState(path string, ws windowState, logger *slog.Logger) {
	data, err := json.Marshal(ws)
	if err != nil {
		return
	}
	if err := os.WriteFile(path, data, 0o600); err != nil {
		logger.Warn("save window state", "error", err)
	}
}
