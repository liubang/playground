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
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"

	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/chromedp"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	defaultNavTimeoutMs  = 30000
	maxNavTimeoutMs      = 120000
	defaultScreenshotFmt = "png"
	// maxInlineImageBytes mirrors view_image/imagegen: base64 inflates by
	// 4/3 and Anthropic caps a single image block at 5MB on the wire, so
	// 3.5MB raw stays below the strictest provider limit. Larger
	// screenshots are still persisted as artifacts, just not inlined into
	// the transcript.
	maxInlineImageBytes = 3584 << 10
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
	// Artifact references the persisted image (raw bytes, with MediaType)
	// when an artifact store is available; the UI renders it for the user.
	Artifact *domain.ArtifactRef `json:"artifact,omitempty"`
	// Inlined reports whether the image is attached to the result as an
	// image part for vision-capable models.
	Inlined bool `json:"inlined"`
	// Note tells the model how the screenshot was delivered so it does not
	// try to re-attach or re-render it.
	Note string `json:"note"`
}

type scrollPosition struct {
	X int `json:"x"`
	Y int `json:"y"`
}

// BrowserTool controls a headless Chrome browser via chromedp. It supports
// seven actions: navigate, snapshot, screenshot, scroll, click, type, and close.
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
// before the tool. A nil artifact store disables screenshot overflow
// (truncated screenshots lose their full data silently).
func NewBrowserTool(manager *Manager, artifacts domain.ArtifactStore, navTimeout time.Duration, screenshotQual int) (*BrowserTool, error) {
	if manager == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "browser manager is required")
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
	return t.base.def
}

// ConcurrentSafe implements domain.ConcurrentSafely: browser operations
// are serialized through the Manager mutex, so concurrent calls won't
// race on the shared chromedp context.
func (t *BrowserTool) ConcurrentSafe() bool { return false }

func (t *BrowserTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[browserArgs](call.Arguments)
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

	return t.base.prepareCall(ctx, call, canonical, approvalDesc, riskForAction(args.Action), urlReq)
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
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := decodeStrict[browserArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
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
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unknown browser action: %q", args.Action)))
	}
}

func (t *BrowserTool) doNavigate(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	browserCtx, err := t.manager.Acquire()
	if err != nil {
		return errorResult(callID, startedAt, err)
	}

	navCtx, cancel := withOpTimeout(ctx, browserCtx, timeout)
	defer cancel()

	var title string
	err = chromedp.Run(navCtx,
		chromedp.Navigate(args.URL),
		chromedp.Title(&title),
	)
	t.manager.Touch()

	if err != nil {
		return errorResult(callID, startedAt, mapBrowserError(err))
	}

	// Invalidate refs: navigation changes the page.
	t.registry.invalidate()

	return successResult(callID, startedAt, browserOutput{
		Action: "navigate",
		URL:    args.URL,
		Title:  title,
		Status: "ok",
	})
}

func (t *BrowserTool) doScreenshot(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	browserCtx, err := t.manager.Acquire()
	if err != nil {
		return errorResult(callID, startedAt, err)
	}

	shotCtx, cancel := withOpTimeout(ctx, browserCtx, timeout)
	defer cancel()

	format := args.Format
	if format == "" {
		format = defaultScreenshotFmt
	}
	quality := args.Quality
	if quality <= 0 {
		quality = t.screenshotQual
	}

	// page.CaptureScreenshot honors format (jpeg quality applies to jpeg
	// only); CaptureBeyondViewport switches viewport → full-page capture.
	var buf []byte
	err = chromedp.Run(shotCtx, chromedp.ActionFunc(func(ctx context.Context) error {
		var err error
		buf, err = page.CaptureScreenshot().
			WithCaptureBeyondViewport(args.FullPage).
			WithFromSurface(true).
			WithFormat(page.CaptureScreenshotFormat(format)).
			WithQuality(int64(quality)).
			Do(ctx)
		return err
	}))
	t.manager.Touch()

	if err != nil {
		return errorResult(callID, startedAt, mapBrowserError(err))
	}

	// Deliver the screenshot the same way imagegen does: persist the raw
	// bytes as an artifact (the UI displays it to the user), and attach an
	// image part for vision-capable models when it fits the inline budget.
	// The base64 payload never goes into the text JSON: there it is dead
	// weight for the model and gets mangled by the generic output
	// truncation layer.
	mediaType := "image/" + format
	payload := &screenshotPayload{
		Format: format,
		Bytes:  len(buf),
	}

	content := make([]domain.ContentPart, 0, 3)
	if ref, ok := t.storeScreenshot(ctx, buf, mediaType); ok {
		payload.Artifact = &ref
		content = append(content, domain.ContentPart{Kind: domain.PartArtifact, Artifact: &ref})
	}
	payload.Inlined = len(buf) <= maxInlineImageBytes
	switch {
	case payload.Artifact != nil && payload.Inlined:
		payload.Note = "The screenshot is persisted as an artifact, already displayed to the user, and attached inline below " +
			"for your review. Do not embed it as a markdown link in your reply."
	case payload.Artifact != nil:
		payload.Note = "The screenshot is persisted as an artifact and already displayed to the user; it exceeds the inline " +
			"size limit, so it is not attached for your review."
	case payload.Inlined:
		payload.Note = "The screenshot is attached inline below for your review and is already displayed to the user."
	default:
		payload.Note = "The screenshot exceeds the inline size limit and no artifact store is configured, so it could not be returned."
	}

	header, err := json.Marshal(browserOutput{
		Action:     "screenshot",
		Screenshot: payload,
		Status:     "ok",
	})
	if err != nil {
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	content = append([]domain.ContentPart{{Kind: domain.PartText, Text: string(header)}}, content...)
	if payload.Inlined {
		content = append(content, domain.ContentPart{Kind: domain.PartImage, Image: &domain.ImageContent{
			MediaType: mediaType,
			Data:      base64.StdEncoding.EncodeToString(buf),
		}})
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
	browserCtx, err := t.manager.Acquire()
	if err != nil {
		return errorResult(callID, startedAt, err)
	}

	scrollCtx, cancel := withOpTimeout(ctx, browserCtx, timeout)
	defer cancel()

	if args.Selector != "" {
		err = chromedp.Run(scrollCtx,
			chromedp.ScrollIntoView(args.Selector, chromedp.ByQuery),
		)
	} else {
		// Use JavaScript to scroll to the specified coordinates.
		script := fmt.Sprintf("window.scrollTo(%d, %d);", args.ScrollX, args.ScrollY)
		err = chromedp.Run(scrollCtx,
			chromedp.Evaluate(script, nil),
		)
	}
	t.manager.Touch()

	if err != nil {
		return errorResult(callID, startedAt, mapBrowserError(err))
	}

	// Read back the current scroll position.
	var scrollX, scrollY int64
	_ = chromedp.Run(scrollCtx,
		chromedp.Evaluate("window.scrollX", &scrollX),
		chromedp.Evaluate("window.scrollY", &scrollY),
	)

	return successResult(callID, startedAt, browserOutput{
		Action: "scroll",
		ScrollPos: &scrollPosition{
			X: int(scrollX),
			Y: int(scrollY),
		},
		Status: "ok",
	})
}

func (t *BrowserTool) doClose(callID domain.ToolCallID, startedAt time.Time) domain.ToolResult {
	t.manager.CloseInstance()
	t.registry.invalidate()
	return successResult(callID, startedAt, browserOutput{
		Action:  "close",
		Status:  "ok",
		Message: "browser instance closed",
	})
}

// storeScreenshot persists the raw image bytes as an artifact with the
// media type set, so the UI can render it for the user. Failure degrades
// to "no artifact" rather than failing the call — the inline image still
// carries the screenshot to the model and the user.
func (t *BrowserTool) storeScreenshot(ctx context.Context, data []byte, mediaType string) (domain.ArtifactRef, bool) {
	if t.artifacts == nil {
		return domain.ArtifactRef{}, false
	}
	stage, err := t.artifacts.Begin(ctx)
	if err != nil {
		return domain.ArtifactRef{}, false
	}
	if _, err := stage.Write(data); err != nil {
		_ = stage.Abort()
		return domain.ArtifactRef{}, false
	}
	if stage.Truncated() {
		_ = stage.Abort()
		return domain.ArtifactRef{}, false
	}
	ref, err := stage.Commit(ctx)
	if err != nil {
		return domain.ArtifactRef{}, false
	}
	ref.MediaType = mediaType
	return ref, true
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

// mapBrowserError translates chromedp errors into domain errors.
func mapBrowserError(err error) error {
	if err == nil {
		return nil
	}
	// Use errors.Is for sentinel errors — chromedp may wrap them.
	if errors.Is(err, context.Canceled) {
		return domain.NewError(domain.ErrCancelled, "browser operation cancelled", domain.WithCause(err))
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return domain.NewError(domain.ErrTimeout, "browser operation timed out", domain.WithCause(err), domain.WithRetryable(true))
	}
	msg := err.Error()
	switch {
	case strings.Contains(msg, "navigation"):
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("navigation failed: %s", msg), domain.WithCause(err), domain.WithRetryable(true))
	case strings.Contains(msg, "connection refused") || strings.Contains(msg, "no such host"):
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("browser connection error: %s", msg), domain.WithCause(err), domain.WithRetryable(true))
	default:
		return domain.NewError(domain.ErrInternal, fmt.Sprintf("browser error: %s", msg), domain.WithCause(err))
	}
}
