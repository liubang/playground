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
	"os/exec"
	"sync"
	"time"

	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/chromedp"
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
// every spoofed property is a surface a page can probe for inconsistency,
// and the launch flags already cover the primary signal
// (navigator.webdriver via --enable-automation removal).
const stealthInitScript = `
Object.defineProperty(navigator, 'webdriver', {get: () => undefined});
Object.defineProperty(navigator, 'languages', {get: () => ['zh-CN', 'zh', 'en']});
Object.defineProperty(navigator, 'plugins', {get: () => [1, 2, 3, 4, 5]});
window.chrome = window.chrome || {runtime: {}};
`

// Manager owns headless Chrome browser instances and reaps idle ones. Each
// workspace gets one Manager; the browser tool delegates to it for all
// chromedp operations. The reaper goroutine periodically checks the
// last-used timestamp of each instance and cancels (closes) ones that
// have been idle beyond the configured TTL, preventing resource leaks
// from long-running sessions.
type Manager struct {
	alloc       context.Context
	allocCancel context.CancelFunc
	cdpURL      string // remote CDP endpoint, empty when launching locally

	mu         sync.Mutex
	instance   *browserInstance
	idleTTL    time.Duration
	reaperDone chan struct{}
	closed     bool
}

type browserInstance struct {
	ctx    context.Context
	cancel context.CancelFunc
	usedAt time.Time
}

// NewManager creates a browser Manager. chromePath is the path to the
// Chrome/Chromium binary; when empty the Manager probes well-known
// locations. idleTTL controls how long an idle browser instance survives
// before the reaper closes it. viewportW/H set the initial window size.
//
// When cdpURL is non-empty, the Manager connects to the remote Chrome
// DevTools Protocol endpoint (ws:// or http://) instead of launching a
// local Chrome process — this lets users point loom at an externally
// managed Chrome (e.g. one with a real profile or anti-detection
// extensions). chromePath is ignored when cdpURL is set.
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

	var allocCtx context.Context
	var cancel context.CancelFunc

	if cdpURL != "" {
		// Remote allocator: connect to an externally managed Chrome.
		// chromedp.NewRemoteAllocator accepts both ws:// and http:// URLs;
		// for http:// it probes /json/version to discover the ws:// endpoint.
		allocCtx, cancel = chromedp.NewRemoteAllocator(context.Background(), cdpURL)
	} else {
		resolved := chromePath
		if resolved == "" {
			resolved = findChrome()
		}
		if resolved == "" {
			return nil, domain.NewError(domain.ErrInvalidInput,
				"Chrome/Chromium binary not found; install Chrome or set browser.chrome_path or browser.cdp_url in config")
		}

		opts := append(
			chromedp.DefaultExecAllocatorOptions[:],
			chromedp.ExecPath(resolved),
			chromedp.WindowSize(viewportW, viewportH),
			// Anti-detection: stock headless Chrome announces automation
			// through --enable-automation (navigator.webdriver=true) and a
			// "HeadlessChrome" user agent, which is exactly what triggers
			// the Baidu/Google bot-verification pages. Strip both markers
			// while keeping headless operation. Flag(name, false) erases a
			// default: chromedp skips boolean flags whose value is false.
			chromedp.Flag("enable-automation", false),
			chromedp.Flag("disable-blink-features", "AutomationControlled"),
			chromedp.UserAgent(stealthUserAgent),
			// Disable GPU and sandbox: headless environments (CI, containers)
			// lack GPU and the user namespace sandbox; these flags are the
			// standard headless workaround.
			chromedp.Flag("disable-gpu", true),
			chromedp.Flag("no-sandbox", true),
		)
		allocCtx, cancel = chromedp.NewExecAllocator(context.Background(), opts...)
	}

	m := &Manager{
		alloc:       allocCtx,
		allocCancel: cancel,
		cdpURL:      cdpURL,
		idleTTL:     idleTTL,
		reaperDone:  make(chan struct{}),
	}

	go m.reaper()
	return m, nil
}

// Acquire returns the current browser instance, creating one if none
// exists. The caller must call touch() after each operation to refresh
// the idle timer.
//
// A newly created instance is cold-started here with its own long-lived
// context: chromedp launches the Chrome process lazily on the first Run
// and ties the process lifetime to the context passed to that Run
// (exec.CommandContext). If the first Run used a per-action timeout
// context, cancelling it after the action would kill Chrome. Starting the
// browser on the instance context keeps the process alive until
// CloseInstance/Close/reaper cancels it.
func (m *Manager) Acquire() (context.Context, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if m.closed {
		return nil, domain.NewError(domain.ErrUnavailable, "browser manager is closed")
	}

	if m.instance != nil {
		// Check if the instance is still alive.
		select {
		case <-m.instance.ctx.Done():
			// Instance died; replace it.
			m.instance = nil
		default:
			return m.instance.ctx, nil
		}
	}

	ctx, cancel := chromedp.NewContext(m.alloc)
	if err := chromedp.Run(ctx); err != nil {
		cancel()
		return nil, domain.NewError(domain.ErrUnavailable, "failed to start browser", domain.WithCause(err))
	}
	// Register the stealth init script before the first navigation, or the
	// document the tool is about to open would miss it. Best-effort: an
	// injection failure only loses the anti-detection hardening — the
	// instance stays usable for pages without bot detection.
	_ = chromedp.Run(ctx, chromedp.ActionFunc(func(ctx context.Context) error {
		_, err := page.AddScriptToEvaluateOnNewDocument(stealthInitScript).Do(ctx)
		return err
	}))
	m.instance = &browserInstance{
		ctx:    ctx,
		cancel: cancel,
		usedAt: time.Now(),
	}
	return ctx, nil
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
		m.instance.cancel()
		m.instance = nil
	}
}

// Close shuts down the Manager: closes any live browser instance, stops
// the reaper goroutine, and releases the Chrome allocator.
func (m *Manager) Close() {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	if m.instance != nil {
		m.instance.cancel()
		m.instance = nil
	}
	m.mu.Unlock()

	close(m.reaperDone)
	m.allocCancel()
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
			m.reapIdle()
		}
	}
}

func (m *Manager) reapIdle() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.instance == nil {
		return
	}
	if time.Since(m.instance.usedAt) > m.idleTTL {
		m.instance.cancel()
		m.instance = nil
	}
}

// findChrome probes well-known locations for a Chrome or Chromium binary.
// Returns the first match, or empty when none found.
func findChrome() string {
	candidates := []string{
		// macOS
		"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
		"/Applications/Chromium.app/Contents/MacOS/Chromium",
		// Linux
		"/usr/bin/google-chrome",
		"/usr/bin/google-chrome-stable",
		"/usr/bin/chromium",
		"/usr/bin/chromium-browser",
		// Windows (Git Bash style paths)
		"C:/Program Files/Google/Chrome/Application/chrome.exe",
		"C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
	}
	for _, c := range candidates {
		if path, err := exec.LookPath(c); err == nil {
			return path
		}
	}
	// Try PATH lookup by bare name.
	for _, name := range []string{"google-chrome", "chromium", "chromium-browser", "chrome"} {
		if path, err := exec.LookPath(name); err == nil {
			return path
		}
	}
	return ""
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
	return fmt.Sprintf("browser.Manager(idleTTL=%s)", m.idleTTL)
}
