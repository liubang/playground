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
	"math"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/chromedp/cdproto/accessibility"
	"github.com/chromedp/cdproto/cdp"
	"github.com/chromedp/cdproto/dom"
	"github.com/chromedp/cdproto/input"
	"github.com/chromedp/chromedp"
	"github.com/chromedp/chromedp/kb"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// maxSnapshotRunes is the token budget for AX tree serialization. Output
// beyond this is truncated with a marker directing the model to scroll
// or narrow scope. See BROWSER_DESIGN.md §5.4.
const maxSnapshotRunes = 8000

// refRegistry holds the mapping between snapshot-assigned ref numbers and
// the AX nodes from the last snapshot. It is scoped to the browser tool
// instance and protected by a mutex because Prepare and Execute can run in
// different goroutines. The registry is replaced on every snapshot and
// invalidated on every action that may change the page (navigate, click,
// type, close).
type refRegistry struct {
	mu   sync.Mutex
	refs map[string]*axNode // ref number (e.g. "[1]") → AX node
}

func newRefRegistry() *refRegistry {
	return &refRegistry{}
}

// axNode is a trimmed view of an accessibility.Node used for ref tracking.
// role and name are kept for error messages when a ref fails to resolve.
type axNode struct {
	backendDOMID cdp.BackendNodeID
	role         string
	name         string
}

// String describes the node for error messages, e.g. `button "Submit"`.
func (n *axNode) String() string {
	if n.name != "" {
		return fmt.Sprintf("%s %q", n.role, n.name)
	}
	return n.role
}

// invalidate clears the registry. Called on navigate/click/type/close to
// force a re-snapshot before the next interaction.
func (r *refRegistry) invalidate() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.refs = nil
}

// lookup returns the AX node for a ref, or nil if the ref is unknown
// (stale or never assigned).
func (r *refRegistry) lookup(ref string) *axNode {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.refs[ref]
}

// resolve returns the AX node for a model-supplied ref. Snapshots print
// refs as "[9]" but models pass both "9" and "[9]"; normalizeRef maps
// both spellings onto the registry key form.
func (r *refRegistry) resolve(ref string) *axNode {
	return r.lookup(normalizeRef(ref))
}

// knownRefs summarizes the live refs for error messages, e.g.
// `[1] link "Download", [2] tab "Leak Suspects"`, so a model that sent
// a bad ref can see what it should have sent. The summary is bounded:
// a large snapshot must not bloat the error.
func (r *refRegistry) knownRefs() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.refs) == 0 {
		return ""
	}
	keys := make([]string, 0, len(r.refs))
	for k := range r.refs {
		keys = append(keys, k)
	}
	// Sort by the numeric part, not lexically: "[9]" < "[10]".
	sort.Slice(keys, func(i, j int) bool {
		if oi, oj := refOrdinal(keys[i]), refOrdinal(keys[j]); oi != oj {
			return oi < oj
		}
		return keys[i] < keys[j]
	})
	const maxListed = 8
	var b strings.Builder
	for i, k := range keys {
		if i == maxListed {
			fmt.Fprintf(&b, ", … (%d more)", len(keys)-maxListed)
			break
		}
		if i > 0 {
			b.WriteString(", ")
		}
		b.WriteString(k)
		b.WriteString(" ")
		b.WriteString(r.refs[k].String())
	}
	return b.String()
}

// normalizeRef canonicalizes a model-supplied element ref to the
// registry key form ("[N]"): trims whitespace and adds the brackets
// the snapshot display format uses.
func normalizeRef(s string) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return s
	}
	if !strings.HasPrefix(s, "[") {
		s = "[" + s
	}
	if !strings.HasSuffix(s, "]") {
		s += "]"
	}
	return s
}

// refOrdinal extracts the numeric part of a ref key ("[9]" → 9) for
// sorting; malformed keys sort last.
func refOrdinal(ref string) int {
	n, err := strconv.Atoi(strings.Trim(ref, "[]"))
	if err != nil {
		return math.MaxInt
	}
	return n
}

// replace atomically swaps the registry contents with a fresh snapshot.
func (r *refRegistry) replace(refs map[string]*axNode) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.refs = refs
}

// --- snapshot action ---

func (t *BrowserTool) doSnapshot(ctx context.Context, callID domain.ToolCallID, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	browserCtx, err := t.manager.Acquire()
	if err != nil {
		return errorResult(callID, startedAt, err)
	}

	snapCtx, cancel := withOpTimeout(ctx, browserCtx, timeout)
	defer cancel()

	// All CDP calls must go through chromedp.Run: it starts the browser on
	// first use and injects the target executor into the context that the
	// cdproto Do methods require.
	var nodes []*accessibility.Node
	err = chromedp.Run(
		snapCtx,
		// Enable the accessibility domain so AXNodeIds are stable.
		accessibility.Enable(),
		chromedp.ActionFunc(func(ctx context.Context) error {
			var err error
			nodes, err = accessibility.GetFullAXTree().Do(ctx)
			return err
		}),
	)
	t.manager.Touch()
	if err != nil {
		return errorResult(callID, startedAt, mapBrowserError(err))
	}

	// Build the serialized tree and populate the ref registry.
	serialized := t.serializeAXTree(nodes)

	return successResult(callID, startedAt, browserOutput{
		Action: "snapshot",
		Status: "ok",
		Output: serialized,
	})
}

// serializeAXTree converts the flat AX node list from CDP into a
// depth-first text serialization with ref numbers for interactive
// elements. It also replaces the tool's refRegistry.
func (t *BrowserTool) serializeAXTree(nodes []*accessibility.Node) string {
	// Build a lookup from AX NodeID → Node for tree reconstruction.
	byID := make(map[accessibility.NodeID]*accessibility.Node, len(nodes))
	var root *accessibility.Node
	for _, n := range nodes {
		byID[n.NodeID] = n
		if n.ParentID == "" {
			if root == nil || n.Role != nil && axString(n.Role.Value) == "RootWebArea" {
				root = n
			}
		}
	}
	if root == nil && len(nodes) > 0 {
		root = nodes[0]
	}
	if root == nil {
		t.registry.replace(nil)
		return "(empty accessibility tree)"
	}

	refs := make(map[string]*axNode)
	refCounter := 0
	var b strings.Builder
	b.WriteString("=== Accessibility Tree Snapshot ===\n\n")

	t.buildAXSerial(&b, root, byID, 0, &refCounter, refs)
	t.registry.replace(refs)

	output := b.String()
	if runeCount(output) > maxSnapshotRunes {
		output = truncateRunes(output, maxSnapshotRunes) +
			"\n\n... (truncated: use scroll or narrower scope to see more)"
	}
	return output
}

// buildAXSerial recursively serializes the AX tree depth-first.
func (t *BrowserTool) buildAXSerial(
	b *strings.Builder,
	node *accessibility.Node,
	byID map[accessibility.NodeID]*accessibility.Node,
	depth int,
	refCounter *int,
	refs map[string]*axNode,
) {
	// Ignored and purely structural nodes contribute no line of their own;
	// their children are hoisted to the same depth to save tokens.
	if node.Ignored || isSkippable(roleOf(node), nameOf(node)) {
		for _, childID := range node.ChildIDs {
			if child, ok := byID[childID]; ok {
				t.buildAXSerial(b, child, byID, depth, refCounter, refs)
			}
		}
		return
	}

	role := roleOf(node)
	name := nameOf(node)

	// Indent for tree structure.
	b.WriteString(strings.Repeat("  ", depth))

	// Assign a ref number to interactive elements.
	if isInteractive(role) && node.BackendDOMNodeID != 0 {
		*refCounter++
		ref := fmt.Sprintf("[%d]", *refCounter)
		b.WriteString(ref)
		b.WriteString(" ")
		refs[ref] = &axNode{
			backendDOMID: node.BackendDOMNodeID,
			role:         role,
			name:         name,
		}
	}

	// Write role and name.
	b.WriteString(role)
	if name != "" {
		b.WriteString(": ")
		b.WriteString(name)
	}

	// Include value for certain roles (e.g., textbox current value).
	if value := axString(nodeValue(node)); value != "" {
		b.WriteString(" (value: ")
		b.WriteString(value)
		b.WriteString(")")
	}

	b.WriteString("\n")

	// Recurse into children.
	for _, childID := range node.ChildIDs {
		if child, ok := byID[childID]; ok {
			t.buildAXSerial(b, child, byID, depth+1, refCounter, refs)
		}
	}
}

// unknownRefError builds the error returned when click/type references a
// ref that is not in the registry. The message must tell the model how to
// recover: the previous wording ("stale or unknown ref; take a new
// snapshot") sent models into a snapshot-and-retry loop when the real
// problem was the ref spelling, because a fresh snapshot re-assigns the
// exact same numbers. Listing the live refs makes the fix obvious.
func (t *BrowserTool) unknownRefError(ref string) error {
	if known := t.registry.knownRefs(); known != "" {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf(
			"unknown ref %q; live refs from the last snapshot: %s. "+
				"Pass one of them (the number alone, e.g. \"3\", also works), "+
				"or take a new snapshot if the page has changed", ref, known,
		))
	}
	return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf(
		"unknown ref %q: no live snapshot refs. Run action=snapshot first, "+
			"then pass the ref number shown next to the target element", ref,
	))
}

// --- click action ---

func (t *BrowserTool) doClick(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	node := t.registry.resolve(args.Ref)
	if node == nil {
		return errorResult(callID, startedAt, t.unknownRefError(args.Ref))
	}

	browserCtx, err := t.manager.Acquire()
	if err != nil {
		return errorResult(callID, startedAt, err)
	}

	clickCtx, cancel := withOpTimeout(ctx, browserCtx, timeout)
	defer cancel()

	// Click the element by its backend DOM node ID (captured at snapshot
	// time). DescribeNode is not usable here: it can return nodeId=0 for
	// nodes the DOM agent has not pushed to the frontend, and chromedp's
	// MouseClickNode requires a frontend nodeId. GetContentQuads accepts a
	// backend node ID directly and fails for detached nodes, which doubles
	// as our stale-element check.
	err = chromedp.Run(clickCtx, clickBackendNode(node.backendDOMID))
	t.manager.Touch()
	if err != nil {
		// Context failures are timeouts/cancellations, not element problems.
		if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return errorResult(callID, startedAt, mapBrowserError(err))
		}
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("failed to click ref %q (%s); the element may be gone or not clickable, take a new snapshot", args.Ref, node),
			domain.WithCause(err)))
	}

	// After click, the page may have changed. Invalidate refs.
	t.registry.invalidate()

	return successResult(callID, startedAt, browserOutput{
		Action:  "click",
		Ref:     args.Ref,
		Status:  "ok",
		Message: t.capturePageSummary(clickCtx),
	})
}

// --- type action ---

func (t *BrowserTool) doType(ctx context.Context, callID domain.ToolCallID, args browserArgs, timeout time.Duration, startedAt time.Time) domain.ToolResult {
	node := t.registry.resolve(args.Ref)
	if node == nil {
		return errorResult(callID, startedAt, t.unknownRefError(args.Ref))
	}

	if args.Text == "" {
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInvalidInput,
			"text is required for type action"))
	}

	browserCtx, err := t.manager.Acquire()
	if err != nil {
		return errorResult(callID, startedAt, err)
	}

	typeCtx, cancel := withOpTimeout(ctx, browserCtx, timeout)
	defer cancel()

	// Focus the element by backend node ID, then insert the text via the
	// IME path (input.InsertText) which handles arbitrary Unicode text.
	err = chromedp.Run(
		typeCtx,
		chromedp.ActionFunc(func(ctx context.Context) error {
			return dom.Focus().WithBackendNodeID(node.backendDOMID).Do(ctx)
		}),
		chromedp.ActionFunc(func(ctx context.Context) error {
			return input.InsertText(args.Text).Do(ctx)
		}),
	)
	if err != nil {
		t.manager.Touch()
		if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return errorResult(callID, startedAt, mapBrowserError(err))
		}
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("failed to type into ref %q (%s); take a new snapshot", args.Ref, node),
			domain.WithCause(err)))
	}

	// If submit is requested, press Enter.
	if args.Submit {
		err = chromedp.Run(typeCtx, chromedp.KeyEvent(kb.Enter))
		if err != nil {
			t.manager.Touch()
			return errorResult(callID, startedAt, mapBrowserError(err))
		}
	}
	t.manager.Touch()

	// After type/submit, the page may have changed. Invalidate refs.
	t.registry.invalidate()

	return successResult(callID, startedAt, browserOutput{
		Action:  "type",
		Ref:     args.Ref,
		Status:  "ok",
		Message: t.capturePageSummary(typeCtx),
	})
}

// clickBackendNode clicks the center of the element identified by a
// backend DOM node ID. It mirrors chromedp.MouseClickNode but keys off the
// backend node ID captured at snapshot time, avoiding a frontend nodeId
// round-trip. The window is scrolled so the element is in the viewport.
func clickBackendNode(backendID cdp.BackendNodeID) chromedp.Action {
	return chromedp.ActionFunc(func(ctx context.Context) error {
		if err := dom.ScrollIntoViewIfNeeded().WithBackendNodeID(backendID).Do(ctx); err != nil {
			return err
		}
		boxes, err := dom.GetContentQuads().WithBackendNodeID(backendID).Do(ctx)
		if err != nil {
			return err
		}
		if len(boxes) == 0 || len(boxes[0]) < 2 || len(boxes[0])%2 != 0 {
			return fmt.Errorf("element has no clickable dimensions")
		}
		content := boxes[0]
		var x, y float64
		for i := 0; i < len(content); i += 2 {
			x += content[i]
			y += content[i+1]
		}
		x /= float64(len(content) / 2)
		y /= float64(len(content) / 2)
		return chromedp.MouseClickXY(x, y).Do(ctx)
	})
}

// capturePageSummary returns a brief post-action page summary (title + URL)
// on a best-effort basis: the page may still be navigating, so failures are
// silently dropped.
func (t *BrowserTool) capturePageSummary(ctx context.Context) string {
	var title, currentURL string
	_ = chromedp.Run(
		ctx,
		chromedp.Title(&title),
		chromedp.Location(&currentURL),
	)
	if title == "" && currentURL == "" {
		return ""
	}
	return fmt.Sprintf("title=%q url=%s", title, currentURL)
}

// --- helpers ---

// axString unmarshals a raw JSON AX value into a plain string. AX values
// arrive as jsontext.Value (raw JSON bytes), so a plain string conversion
// would keep the surrounding quotes. Non-string scalars (numbers, bools)
// are returned as-is.
func axString(raw []byte) string {
	if len(raw) == 0 {
		return ""
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return string(raw)
	}
	return s
}

// nodeValue safely extracts the raw JSON of an AX node's value.
func nodeValue(n *accessibility.Node) []byte {
	if n == nil || n.Value == nil {
		return nil
	}
	return n.Value.Value
}

// roleOf safely extracts the role value from an AX node.
func roleOf(n *accessibility.Node) string {
	if n == nil || n.Role == nil {
		return "unknown"
	}
	if s := axString(n.Role.Value); s != "" {
		return s
	}
	return "unknown"
}

// nameOf safely extracts the accessible name value from an AX node.
func nameOf(n *accessibility.Node) string {
	if n == nil || n.Name == nil {
		return ""
	}
	return axString(n.Name.Value)
}

// isInteractive reports whether the AX role represents an element the
// user can interact with (and thus should get a ref number). Chrome's role
// casing is inconsistent across versions (e.g. "textbox" vs "textBox"),
// so the match is case-insensitive.
func isInteractive(role string) bool {
	switch strings.ToLower(role) {
	case "button", "link", "textbox", "searchbox", "combobox",
		"checkbox", "radiobutton", "slider", "spinbutton",
		"menuitem", "menuitemcheckbox", "menuitemradio",
		"tab", "switch", "treeitem", "option":
		return true
	default:
		return false
	}
}

// isSkippable reports whether a node should be omitted from the
// serialization to save tokens (purely structural/intermediate nodes).
// Like isInteractive, the match is case-insensitive.
func isSkippable(role, name string) bool {
	// Keep nodes with names — they carry information.
	if name != "" {
		return false
	}
	switch strings.ToLower(role) {
	case "none", "generic", "inlinetextbox", "linebreak",
		"layouttable", "layouttablecell", "layouttablerow":
		return true
	default:
		return false
	}
}

// runeCount returns the number of runes in a string.
func runeCount(s string) int {
	return len([]rune(s))
}

// truncateRunes returns the first n runes of s.
func truncateRunes(s string, n int) string {
	runes := []rune(s)
	if len(runes) <= n {
		return s
	}
	return string(runes[:n])
}
