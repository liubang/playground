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
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/proto"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/media"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

const (
	defaultNavTimeoutMs  = 30000
	maxNavTimeoutMs      = 120000
	defaultScreenshotFmt = "png"
)

// browserArgs is the validated argument set for the browser tool.
type browserArgs struct {
	Action    string `json:"action"`
	URL       string `json:"url,omitempty"`
	Ref       string `json:"ref,omitempty"`
	Text      string `json:"text,omitempty"`
	Submit    bool   `json:"submit,omitempty"`
	Selector  string `json:"selector,omitempty"`
	ScrollX   int    `json:"scroll_x,omitempty"`
	ScrollY   int    `json:"scroll_y,omitempty"`
	Format    string `json:"format,omitempty"`
	Quality   int    `json:"quality,omitempty"`
	FullPage  bool   `json:"full_page,omitempty"`
	TimeoutMs int    `json:"timeout_ms,omitempty"`
}

// browserOutput is the result shape returned to the model.
type browserOutput struct {
	Action     string             `json:"action"`
	URL        string             `json:"url,omitempty"`
	Title      string             `json:"title,omitempty"`
	Status     string             `json:"status,omitempty"`
	Screenshot *screenshotPayload `json:"screenshot,omitempty"`
	ScrollPos  *scrollPosition    `json:"scroll_position,omitempty"`
	Output     string             `json:"output,omitempty"`
	Ref        string             `json:"ref,omitempty"`
	Message    string             `json:"message,omitempty"`
}

type screenshotPayload struct {
	Format string `json:"format"`
	Bytes  int    `json:"bytes"`
	// Artifact references the persisted image (raw bytes, with MediaType);
	// the UI renders it for the user, and the agent loop materializes it
	// into a derived inline image for the model (media.Materialize).
	Artifact *domain.ArtifactRef `json:"artifact,omitempty"`
	// Note tells the model how the screenshot was delivered so it does not
	// try to re-attach or re-render it.
	Note string `json:"note"`
}

type scrollPosition struct {
	X int `json:"x"`
	Y int `json:"y"`
}

// BrowserTool controls a headless Chrome browser via go-rod (CDP). It
// supports seven actions: navigate, snapshot, screenshot, scroll, click,
// type, and close.
// The tool carries a Manager that owns the browser instance lifecycle (idle-TTL
// reaping) and a refRegistry that maps snapshot-assigned ref numbers to AX
// node IDs for click/type operations.
type BrowserTool struct {
	base      baseTool
	manager   *Manager
	artifacts domain.ArtifactStore
	registry  *refRegistry
	// Config-baked defaults
	navTimeout     time.Duration
	screenshotQual int
}

// NewBrowserTool creates the browser tool. The Manager must be created
// before the tool. The artifact store is required: screenshots are
// persisted as artifacts and materialized for the model at the egress
// (media.Materialize), so without it the screenshot action would have no
// delivery channel at all.
func NewBrowserTool(manager *Manager, artifacts domain.ArtifactStore, navTimeout time.Duration, screenshotQual int) (*BrowserTool, error) {
	if manager == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "browser manager is required")
	}
	if artifacts == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "browser artifact store is required")
	}
	if navTimeout <= 0 {
		navTimeout = defaultNavTimeoutMs * time.Millisecond
	}
	if screenshotQual <= 0 {
		screenshotQual = defaultScreenshotQuality
	}
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "browser",
		Description: "Control a headless Chrome browser to navigate web pages, take screenshots, snapshot the accessibility tree, scroll, click, type, and close. " +
			"Use it when web_fetch is insufficient (JavaScript-rendered content, visual inspection, SPAs). " +
			"Actions: navigate (open a URL), snapshot (get AX tree with ref numbers), screenshot (capture the page), scroll (move the viewport), click (click an element by ref), type (enter text into an element by ref), close (release the browser). " +
			"The browser instance persists across calls and is automatically reaped after 5 minutes of inactivity.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["navigate","snapshot","screenshot","scroll","click","type","close"],"description":"The browser action to perform"},"url":{"type":"string","minLength":1,"maxLength":2048,"description":"URL to navigate to (required for navigate)"},"ref":{"type":"string","description":"Element ref from snapshot (required for click/type)"},"text":{"type":"string","maxLength":10000,"description":"Text to type into element (required for type)"},"submit":{"type":"boolean","description":"Press Enter after typing (optional for type)"},"selector":{"type":"string","description":"CSS selector to scroll to (optional for scroll)"},"scroll_x":{"type":"integer","description":"Horizontal scroll offset in pixels"},"scroll_y":{"type":"integer","description":"Vertical scroll offset in pixels"},"format":{"type":"string","enum":["png","jpeg"],"description":"Screenshot format (default: png)"},"quality":{"type":"integer","minimum":10,"maximum":100,"description":"JPEG quality (default: 80)"},"full_page":{"type":"boolean","description":"Capture full page instead of viewport (default: false)"},"timeout_ms":{"type":"integer","minimum":5000,"maximum":120000,"description":"Per-action timeout in milliseconds"}},"required":["action"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"action":{"type":"string"},"url":{"type":"string"},"title":{"type":"string"},"status":{"type":"string"},"screenshot":{"type":"object"},"scroll_position":{"type":"object"},"output":{"type":"string"},"ref":{"type":"string"},"message":{"type":"string"}},"required":["action","status"]}`),
		// No network.connect capability: it maps to a static R3 floor, while
		// this tool grades risk PER ACTION in Prepare (docs/BROWSER_DESIGN.md
		// §5.2: snapshot/screenshot/scroll/click are R2, navigate/type R3) —
		// and the loop's execution-time drift guard rejects a prepared risk
		// BELOW the definition's static tier. The per-action elevation is
		// covered by the prepared-call signature, the same shape as run_cmd's
		// riskForArgs and delegate_task's riskOf. Source=builtin is the audit
		// marker.
		Source: domain.ToolSourceBuiltin,
	})
	if err != nil {
		return nil, err
	}
	return &BrowserTool{
		base:           base,
		manager:        manager,
		artifacts:      artifacts,
		registry:       newRefRegistry(),
		navTimeout:     navTimeout,
		screenshotQual: screenshotQual,
	}, nil
}

// defaultScreenshotQuality is the JPEG quality used when the caller does
// not specify one.
const defaultScreenshotQuality = 80

func (t *BrowserTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: browser operations
// mutate one shared page, so calls must be serialized by the agent loop.
func (t *BrowserTool) ConcurrentSafe() bool { return false }

// pageFor acquires the instance page and derives the per-action operation
// handle: the returned page honors both cancellation of the caller's
// context (user interrupting the agent loop) and the action timeout. The
// release func cancels the timeout and refreshes the instance idle timer;
// callers must defer it.
func (t *BrowserTool) pageFor(ctx context.Context, timeout time.Duration) (*rod.Page, func(), error) {
	page, err := t.manager.Acquire()
	if err != nil {
		return nil, nil, err
	}
	op := page.Context(ctx).Timeout(timeout)
	return op, func() {
		op.CancelTimeout()
		t.manager.Touch()
	}, nil
}

func (t *BrowserTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeStrict[browserArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, err = validateBrowserArgs(args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}

	approvalDesc := fmt.Sprintf("browser %s", args.Action)
	if args.URL != "" {
		approvalDesc = fmt.Sprintf("browser %s %s", args.Action, args.URL)
	}

	// Only navigate carries a URLRequest for domain-rule evaluation.
	var urlReq *domain.URLRequest
	if args.URL != "" {
		urlReq = extractURLRequest(args.URL)
	}

	risk := riskForAction(args.Action)
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{
		ApprovalDesc: approvalDesc,
		Risk:         &risk,
		URLRequest:   urlReq,
	})
}

// riskForAction grades the call's risk by action (docs/BROWSER_DESIGN.md
// §5.2, review M4): read/shape operations on the ALREADY-APPROVED page
// (snapshot, screenshot, scroll, click, close) are R2 and run without
// prompting in every approval mode, while actions that inject data or
// cross an origin (type, navigate) stay R3 — navigate additionally flows
// through the domain rule set via its URLRequest. Grading per action is
// what keeps the tool usable: a flat R3 would demand an approval for
// every single step of a browsing session.
func riskForAction(action string) domain.RiskLevel {
	switch action {
	case "navigate", "type":
		return domain.R3
	default:
		// snapshot, screenshot, scroll, click, close. validateBrowserArgs
		// has already rejected unknown actions by the time this runs.
		return domain.R2
	}
}

func (t *BrowserTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := toolkit.DecodeStrict[browserArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	// Per-action timeout.
	timeout := t.navTimeout
	if args.TimeoutMs > 0 {
		timeout = time.Duration(args.TimeoutMs) * time.Millisecond
	}

	switch args.Action {
	case "navigate":
		return t.doNavigate(ctx, prepared.Call.ID, args, timeout, startedAt)
	case "snapshot":
		return t.doSnapshot(ctx, prepared.Call.ID, timeout, startedAt)
	case "screenshot":
		return t.doScreenshot(ctx, prepared.Call.ID, args, timeout, startedAt)
	case "scroll":
		return t.doScroll(ctx, prepared.Call.ID, args, timeout, startedAt)
	case "click":
		return t.doClick(ctx, prepared.Call.ID, args, timeout, startedAt)
	case "type":
		return t.doType(ctx, prepared.Call.ID, args, timeout, startedAt)
	case "close":
		return t.doClose(prepared.Call.ID, startedAt)
	default:
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unknown browser action: %q", args.Action)))
	}
}

func (t *BrowserTool) doNavigate(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	op, release, err := t.pageFor(ctx, timeout)
	if err != nil {
		return toolkit.ErrorResult(callID, startedAt, err)
	}
	defer release()

	// A fragment-only change is a same-document navigation: Chrome fires
	// no load event, the page does not reload, and history-mode SPAs
	// (Vue/React Router) ignore the fragment entirely. Waiting for a load
	// event that never comes would burn the whole action timeout, so
	// same-document navigations skip WaitLoad — and the model must be told
	// the click path is the only way through, instead of receiving a bare
	// "ok" for a navigation that changed nothing.
	var prevURL string
	if info, err := op.Info(); err == nil {
		prevURL = info.URL
	}
	sameDoc := prevURL != "" && sameDocumentURL(prevURL, args.URL)

	if err := op.Navigate(args.URL); err != nil {
		return toolkit.ErrorResult(callID, startedAt, mapBrowserError(err))
	}
	if !sameDoc {
		// WaitLoad returns immediately when the load event already fired.
		if err := op.WaitLoad(); err != nil {
			return toolkit.ErrorResult(callID, startedAt, mapBrowserError(err))
		}
	}

	// Invalidate refs: navigation changes the page.
	t.registry.invalidate()

	var title string
	if info, err := op.Info(); err == nil {
		title = info.Title
	}

	out := browserOutput{
		Action: "navigate",
		URL:    args.URL,
		Title:  title,
		Status: "ok",
	}
	if sameDoc {
		out.Message = "only the URL fragment changed and the document did not reload; " +
			"if the page content did not switch, this SPA uses history-mode routing — " +
			"snapshot the page and click the target element by ref instead of navigating by fragment"
	}
	return toolkit.SuccessResult(callID, startedAt, out)
}

// sameDocumentURL reports whether two URLs address the same document,
// i.e. they differ at most in the fragment.
func sameDocumentURL(a, b string) bool {
	ua, errA := url.Parse(a)
	ub, errB := url.Parse(b)
	if errA != nil || errB != nil {
		return false
	}
	ua.Fragment, ua.RawFragment = "", ""
	ub.Fragment, ub.RawFragment = "", ""
	return ua.String() == ub.String()
}

func (t *BrowserTool) doScreenshot(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	op, release, err := t.pageFor(ctx, timeout)
	if err != nil {
		return toolkit.ErrorResult(callID, startedAt, err)
	}
	defer release()

	format := args.Format
	if format == "" {
		format = defaultScreenshotFmt
	}
	quality := args.Quality
	if quality <= 0 {
		quality = t.screenshotQual
	}

	// Page.captureScreenshot honors format (jpeg quality applies to jpeg
	// only); CaptureBeyondViewport switches viewport → full-page capture.
	shot, err := proto.PageCaptureScreenshot{
		Format:                proto.PageCaptureScreenshotFormat(format),
		Quality:               &quality,
		FromSurface:           true,
		CaptureBeyondViewport: args.FullPage,
	}.Call(op)
	if err != nil {
		return toolkit.ErrorResult(callID, startedAt, mapBrowserError(err))
	}
	buf := shot.Data

	// Deliver the screenshot the same way every image source does: persist
	// the raw bytes as an artifact. The UI renders the reference for the
	// user; the agent loop materializes it into a derived (rescaled) inline
	// image for the model at request time. Nothing is ever inlined here —
	// base64 in the transcript is dead weight and gets mangled by the
	// generic output truncation layer.
	ref, err := media.StoreImage(ctx, t.artifacts, buf)
	if err != nil {
		return toolkit.ErrorResult(callID, startedAt, err)
	}
	payload := &screenshotPayload{
		Format:   format,
		Bytes:    len(buf),
		Artifact: &ref,
		Note: "The screenshot is persisted as an artifact, already displayed to the user, and attached for your " +
			"review. Do not embed it as a markdown link in your reply.",
	}

	header, err := json.Marshal(browserOutput{
		Action:     "screenshot",
		Screenshot: payload,
		Status:     "ok",
	})
	if err != nil {
		return toolkit.ErrorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	content := []domain.ContentPart{
		{Kind: domain.PartText, Text: string(header)},
		{Kind: domain.PartArtifact, Artifact: &ref},
	}

	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusSuccess,
		Content:    content,
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

func (t *BrowserTool) doScroll(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	op, release, err := t.pageFor(ctx, timeout)
	if err != nil {
		return toolkit.ErrorResult(callID, startedAt, err)
	}
	defer release()

	if args.Selector != "" {
		// Element retries the query until the action timeout, covering
		// elements that render late on JS-heavy pages.
		el, err := op.Element(args.Selector)
		if err != nil {
			return toolkit.ErrorResult(callID, startedAt, mapBrowserError(err))
		}
		if err := el.ScrollIntoView(); err != nil {
			return toolkit.ErrorResult(callID, startedAt, mapBrowserError(err))
		}
	} else {
		// Coordinates go through rod's argument passing, not string
		// interpolation into JS source.
		if _, err := op.Eval("(x, y) => window.scrollTo(x, y)", args.ScrollX, args.ScrollY); err != nil {
			return toolkit.ErrorResult(callID, startedAt, mapBrowserError(err))
		}
	}

	// Read back the current scroll position, best-effort: the scroll
	// itself already succeeded, so a failed read must not fail the action.
	var scrollX, scrollY int
	if res, err := op.Eval("() => [window.scrollX, window.scrollY]"); err == nil {
		if pos := res.Value.Arr(); len(pos) == 2 {
			scrollX, scrollY = pos[0].Int(), pos[1].Int()
		}
	}

	return toolkit.SuccessResult(callID, startedAt, browserOutput{
		Action:    "scroll",
		ScrollPos: &scrollPosition{X: scrollX, Y: scrollY},
		Status:    "ok",
	})
}

func (t *BrowserTool) doClose(callID domain.ToolCallID, startedAt time.Time) domain.ToolResult {
	t.manager.CloseInstance()
	t.registry.invalidate()
	return toolkit.SuccessResult(callID, startedAt, browserOutput{
		Action:  "close",
		Status:  "ok",
		Message: "browser instance closed",
	})
}

// validateBrowserArgs normalizes and validates the call arguments.
func validateBrowserArgs(args browserArgs) (browserArgs, error) {
	switch args.Action {
	case "navigate", "snapshot", "screenshot", "scroll", "click", "type", "close":
	default:
		return browserArgs{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("action must be one of navigate, snapshot, screenshot, scroll, click, type, close; got %q", args.Action))
	}

	if args.Action == "navigate" {
		if strings.TrimSpace(args.URL) == "" {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput, "url is required for navigate action")
		}
		u, err := url.Parse(args.URL)
		if err != nil {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput, "url is not a valid URL", domain.WithCause(err))
		}
		if u.Scheme != "http" && u.Scheme != "https" {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("url scheme must be http or https; got %q", u.Scheme))
		}
		if u.Hostname() == "" {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput, "url must include a host")
		}
	}

	if args.Action == "click" {
		if strings.TrimSpace(args.Ref) == "" {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput, "ref is required for click action")
		}
	}

	if args.Action == "type" {
		if strings.TrimSpace(args.Ref) == "" {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput, "ref is required for type action")
		}
		if args.Text == "" {
			return browserArgs{}, domain.NewError(domain.ErrInvalidInput, "text is required for type action")
		}
	}

	if args.Format != "" && args.Format != "png" && args.Format != "jpeg" {
		return browserArgs{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("format must be png or jpeg; got %q", args.Format))
	}

	if args.TimeoutMs != 0 && (args.TimeoutMs < 5000 || args.TimeoutMs > maxNavTimeoutMs) {
		return browserArgs{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("timeout_ms must be between 5000 and %d", maxNavTimeoutMs))
	}

	if args.Quality != 0 && (args.Quality < 10 || args.Quality > 100) {
		return browserArgs{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("quality must be between 10 and 100"))
	}

	// Normalize: empty action is invalid (already caught above).
	return args, nil
}

// mapBrowserError translates rod errors into domain errors.
func mapBrowserError(err error) error {
	if err == nil {
		return nil
	}
	// Use errors.Is/As for the typed failures — rod wraps them with the
	// CDP method name.
	if errors.Is(err, context.Canceled) {
		return domain.NewError(domain.ErrCancelled, "browser operation cancelled", domain.WithCause(err))
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return domain.NewError(domain.ErrTimeout, "browser operation timed out", domain.WithCause(err), domain.WithRetryable(true))
	}
	var navErr *rod.NavigationError
	if errors.As(err, &navErr) {
		return domain.NewError(domain.ErrUnavailable, navErr.Error(), domain.WithCause(err), domain.WithRetryable(true))
	}
	msg := err.Error()
	switch {
	case strings.Contains(msg, "connection refused") || strings.Contains(msg, "no such host"):
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("browser connection error: %s", msg), domain.WithCause(err), domain.WithRetryable(true))
	default:
		return domain.NewError(domain.ErrInternal, fmt.Sprintf("browser error: %s", msg), domain.WithCause(err))
	}
}
