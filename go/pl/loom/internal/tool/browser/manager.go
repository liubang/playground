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
	"fmt"
	"sync"
	"time"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/launcher"
	"github.com/go-rod/rod/lib/proto"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// stealthUserAgent replaces the stock headless UA ("HeadlessChrome/…")
// with an ordinary desktop Chrome UA — the "Headless" marker alone is
// enough for Baidu/Google risk control to serve a verification page.
// The exact build number does not matter; the absence of the marker does.
const stealthUserAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) " +
	"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

// stealthInitScript runs in every new document before any page script
// (Page.addScriptToEvaluateOnNewDocument), erasing the automation
// fingerprints a CDP-driven Chrome otherwise exposes. Keep it minimal:
// every spoofed property is a surface a page can probe for inconsistency.
const stealthInitScript = `
Object.defineProperty(navigator, 'webdriver', {get: () => undefined});
Object.defineProperty(navigator, 'languages', {get: () => ['zh-CN', 'zh', 'en']});
Object.defineProperty(navigator, 'plugins', {get: () => [1, 2, 3, 4, 5]});
window.chrome = window.chrome || {runtime: {}};
`

// closeFarewell bounds the tab-closing round-trip when an instance is
// torn down, so a hung browser cannot stall the Manager mutex.
const closeFarewell = 3 * time.Second

// Manager owns the headless browser instance and reaps it after an idle
// TTL. Each workspace gets one Manager; the browser tool delegates to it
// for all rod operations. Every action works on the instance's single
// page: the tool definition models one browsing session, not a tab strip.
type Manager struct {
	binPath   string // resolved Chrome/Chromium binary; empty in remote mode
	cdpURL    string // remote CDP endpoint, empty when launching locally
	viewportW int
	viewportH int
	idleTTL   time.Duration

	mu         sync.Mutex
	instance   *instance
	reaperDone chan struct{}
	closed     bool
}

// instance bundles one browser run: the rod controller, the page every
// action operates on, and the teardown hooks. cancel drops the CDP
// connection (rod's client lifetime is bound to this context); cleanup is
// local-mode only: it kills the Chrome process group and removes the
// throwaway profile dir, and it BLOCKS until the process exits — callers
// must never run it under the Manager mutex.
type instance struct {
	browser *rod.Browser
	page    *rod.Page
	cancel  context.CancelFunc
	cleanup func()
	usedAt  time.Time
}

// NewManager creates a browser Manager. chromePath is the path to the
// Chrome/Chromium binary; when empty the well-known install locations are
// probed. idleTTL controls how long an idle instance survives before the
// reaper closes it. viewportW/H set the initial window size.
//
// When cdpURL is non-empty, the Manager connects to this remote Chrome
// DevTools Protocol endpoint (ws:// or http://) instead of launching a
// local process — letting users point loom at an externally managed
// Chrome (e.g. a containerized Chromium or one with a real profile and
// anti-detection extensions). chromePath is ignored when cdpURL is set.
func NewManager(chromePath, cdpURL string, idleTTL time.Duration, viewportW, viewportH int) (*Manager, error) {
	if idleTTL <= 0 {
		idleTTL = 5 * time.Minute
	}
	if viewportW <= 0 {
		viewportW = 1280
	}
	if viewportH <= 0 {
		viewportH = 720
	}

	m := &Manager{
		cdpURL:     cdpURL,
		viewportW:  viewportW,
		viewportH:  viewportH,
		idleTTL:    idleTTL,
		reaperDone: make(chan struct{}),
	}

	if cdpURL == "" {
		m.binPath = chromePath
		if m.binPath == "" {
			m.binPath, _ = launcher.LookPath()
		}
		if m.binPath == "" {
			return nil, domain.NewError(domain.ErrInvalidInput,
				"Chrome/Chromium binary not found; install Chrome or set browser.chrome_path or browser.cdp_url in config")
		}
	}

	go m.reaper()
	return m, nil
}

// Acquire returns the instance page, creating the instance on first use.
// The caller must call Touch after each operation to refresh the idle
// timer — pageFor in browser.go folds both into one helper.
func (m *Manager) Acquire() (*rod.Page, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if m.closed {
		return nil, domain.NewError(domain.ErrUnavailable, "browser manager is closed")
	}

	if m.instance != nil {
		if m.instance.alive() {
			return m.instance.page, nil
		}
		// The browser died (crash, kill, remote endpoint gone): drop the
		// corpse and start fresh instead of failing every future action.
		m.instance.stop()
		m.instance = nil
	}

	inst, err := m.start()
	if err != nil {
		return nil, err
	}
	m.instance = inst
	return inst.page, nil
}

// Touch refreshes the last-used timestamp of the current instance.
func (m *Manager) Touch() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.instance != nil {
		m.instance.usedAt = time.Now()
	}
}

// CloseInstance forcibly closes the current browser instance. Used by
// the "close" action to release resources immediately.
func (m *Manager) CloseInstance() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.instance != nil {
		m.instance.stop()
		m.instance = nil
	}
}

// Close shuts down the Manager: closes any live instance and stops the
// reaper goroutine.
func (m *Manager) Close() {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	if m.instance != nil {
		m.instance.stop()
		m.instance = nil
	}
	m.mu.Unlock()

	close(m.reaperDone)
}

// start launches (or attaches to) a browser and opens the instance page.
// On any failure the partially started instance is fully torn down, so a
// failed Acquire never leaks a process or a connection.
func (m *Manager) start() (*instance, error) {
	ctx, cancel := context.WithCancel(context.Background())
	inst := &instance{cancel: cancel, usedAt: time.Now()}

	browser := rod.New().NoDefaultDevice().Context(ctx)
	if m.cdpURL != "" {
		// Remote mode: ResolveURL normalizes host:port / http(s):// /
		// ws(s):// to the browser-level websocket endpoint by probing
		// /json/version.
		controlURL, err := launcher.ResolveURL(m.cdpURL)
		if err != nil {
			cancel()
			return nil, domain.NewError(domain.ErrUnavailable,
				fmt.Sprintf("failed to resolve CDP endpoint %q", m.cdpURL), domain.WithCause(err))
		}
		browser = browser.ControlURL(controlURL)
	} else {
		// Local mode: rod's launcher defaults already cover headless,
		// leakless (Chrome dies with the connection) and a random debug
		// port. What remains is the binary, the viewport, and the
		// anti-detection hardening: the stock headless UA and the
		// AutomationControlled blink feature are exactly what bot
		// detection keys on.
		l := launcher.New().
			Bin(m.binPath).
			Set("user-agent", stealthUserAgent).
			Set("window-size", fmt.Sprintf("%d,%d", m.viewportW, m.viewportH)).
			Set("disable-blink-features", "AutomationControlled").
			Set("disable-gpu").
			Set("no-sandbox")
		controlURL, err := l.Launch()
		if err != nil {
			// rod already kills a spawned-but-unreachable browser on Launch
			// failure; only the connection context needs releasing here.
			cancel()
			return nil, domain.NewError(domain.ErrUnavailable,
				"failed to launch Chrome", domain.WithCause(err))
		}
		browser = browser.ControlURL(controlURL)
		inst.cleanup = func() {
			l.Kill()
			l.Cleanup()
		}
	}

	if err := browser.Connect(); err != nil {
		inst.stop()
		return nil, domain.NewError(domain.ErrUnavailable,
			"failed to connect to browser", domain.WithCause(err))
	}
	page, err := browser.Page(proto.TargetCreateTarget{})
	if err != nil {
		inst.stop()
		return nil, domain.NewError(domain.ErrUnavailable,
			"failed to open browser tab", domain.WithCause(err))
	}
	inst.browser = browser
	inst.page = page

	// Register the stealth init script before the first navigation, or the
	// document the tool is about to open would miss it. Best-effort: an
	// injection failure only loses the anti-detection hardening — the
	// instance stays usable for pages without bot detection.
	_, _ = page.EvalOnNewDocument(stealthInitScript)

	return inst, nil
}

// alive probes the CDP connection with a lightweight round-trip. A dead
// browser (crashed process, closed remote endpoint) fails the probe and
// the instance is replaced on the next Acquire.
func (i *instance) alive() bool {
	_, err := proto.BrowserGetVersion{}.Call(i.browser)
	return err == nil
}

// stop tears the instance down without ever blocking the Manager mutex.
// Remote mode closes only our tab — the shared browser stays alive — with
// a bounded farewell so a hung endpoint cannot stall the mutex; the local
// Chrome needs no farewell because cleanup kills the whole process group,
// off the mutex.
func (i *instance) stop() {
	if i.cleanup == nil && i.page != nil {
		farewell, cancel := context.WithTimeout(context.Background(), closeFarewell)
		_ = i.page.Context(farewell).Close()
		cancel()
	}
	i.cancel()
	if i.cleanup != nil {
		go i.cleanup()
	}
}

// reaper periodically checks for idle browser instances and closes
// ones that have not been used within the idle TTL.
func (m *Manager) reaper() {
	ticker := time.NewTicker(m.idleTTL / 2)
	defer ticker.Stop()

	for {
		select {
		case <-m.reaperDone:
			return
		case <-ticker.C:
			m.mu.Lock()
			if m.instance != nil && time.Since(m.instance.usedAt) > m.idleTTL {
				m.instance.stop()
				m.instance = nil
			}
			m.mu.Unlock()
		}
	}
}

// CdpURL returns the remote CDP endpoint when the Manager is connected to
// an external Chrome, or empty when launching a local Chrome process.
func (m *Manager) CdpURL() string {
	return m.cdpURL
}

// String returns a human-readable description for logging.
func (m *Manager) String() string {
	if m.cdpURL != "" {
		return fmt.Sprintf("browser.Manager(remote=%s, idleTTL=%s)", m.cdpURL, m.idleTTL)
	}
	return fmt.Sprintf("browser.Manager(bin=%s, idleTTL=%s)", m.binPath, m.idleTTL)
}
