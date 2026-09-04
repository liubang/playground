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

package harness

import (
	"encoding/json"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/replay"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Snapshot golden normalization (REPLAY_TESTING_DESIGN §5.1). Two views
// of a run are normalized into stable text and diffed against committed
// golden files: the broker's runtime event stream and the persisted
// transcripts. Volatile values are rewritten deterministically:
//
//   - loom-minted IDs (sess_/run_/msg_/evt_/tc_/art_/ckpt_/ws_/turn_)
//     become {{id:1}}, {{id:2}}, … in first-appearance order — the SAME
//     raw value always maps to the same token, so cross-references stay
//     verifiable;
//   - timestamps become 0;
//   - the broker-global event sequence becomes 0 (per-session persisted
//     sequences stay — they are deterministic; the broker's is not,
//     under concurrent sub-agents);
//   - the workspace / artifact-dir / loom-home path prefixes become
//     {{cwd}} / {{artifacts}} / {{home}}, boundary-aware so a sharing
//     prefix cannot corrupt a longer path;
//   - usage numbers stay: they are deterministic under replay and are a
//     valid assertion.

// loomIDRe matches the IDs loom mints (newID: prefix + "_" + 16 random
// hex bytes). Provider-issued tool call IDs are deliberately NOT
// rewritten: replay emits the recorded IDs verbatim, so they are stable
// — and the sub-agent fixture binding depends on them.
var loomIDRe = regexp.MustCompile(`^(sess|run|turn|msg|tc|evt|art|ckpt|ws)_[0-9a-f]{32}$|^ctxm_[0-9a-f]{24}$`)

// loomIDEmbeddedRe matches the same IDs embedded inside larger strings
// (tool-result previews carry {"child_session_id":"sess_..."} as text).
// Each distinct match is rewritten via the shared first-appearance
// ordinals, so embedded references stay linked with their field-valued
// occurrences.
var loomIDEmbeddedRe = regexp.MustCompile(`(sess|run|turn|msg|tc|evt|art|ckpt|ws)_[0-9a-f]{32}|ctxm_[0-9a-f]{24}`)

// hexVolatileRe matches content hashes and trace identities
// (prompt_hash, manifest hash, args_hash, trace_id, …). They are
// digests over pre-normalization content (dates, temp paths) or random
// per run, so they can never survive a record/replay pair verbatim; each
// distinct value is rewritten in first-appearance order so repeated
// references stay linked.
var hexVolatileRe = regexp.MustCompile(`^([0-9a-f]{32}|[0-9a-f]{64}|sha256:[0-9a-f]{64})$`)

// timeKeys are JSON object keys whose values are wall-clock timestamps
// or run-duration noise.
var timeKeys = map[string]bool{
	"time":        true,
	"timestamp":   true,
	"created_at":  true,
	"updated_at":  true,
	"started_at":  true,
	"finished_at": true,
	"duration_ms": true,
	// delta_bytes derives from the length of a streamed argument fragment,
	// which embeds the run's (differently-long) temp paths.
	"delta_bytes": true,
	// occupancy_tokens mixes the provider-metered footprint (replayed
	// verbatim, stable) with a byte-estimate of message text, which
	// embeds the run's temp paths — the sum is not stable either.
	"occupancy_tokens": true,
	// WallTime (domain.Usage, wall_time_ns) is run-duration noise.
	"wall_time_ns": true,
}

// NormalizeContext carries the run's environment-dependent path roots.
type NormalizeContext struct {
	Workspace   string
	ArtifactDir string
	Home        string
}

type normalizer struct {
	ctx    NormalizeContext
	ids    map[string]int
	hashes map[string]int
	paths  []pathToken
}

type pathToken struct {
	prefix string
	token  string
}

func newNormalizer(ctx NormalizeContext) *normalizer {
	n := &normalizer{ctx: ctx, ids: make(map[string]int), hashes: make(map[string]int)}
	for _, p := range []pathToken{
		{ctx.ArtifactDir, "{{artifacts}}"},
		{ctx.Workspace, "{{cwd}}"},
		{ctx.Home, "{{home}}"},
	} {
		if p.prefix == "" {
			continue
		}
		n.paths = append(n.paths, p)
		// macOS reports one temp dir in two spellings: t.TempDir() is
		// /var/... while canonicalized tool paths are /private/var/...
		if strings.HasPrefix(p.prefix, "/") && !strings.HasPrefix(p.prefix, "/private/") {
			n.paths = append(n.paths, pathToken{"/private" + p.prefix, p.token})
		}
	}
	// Longest first: a nested root (artifacts under home) must win over
	// its shorter ancestor.
	sort.Slice(n.paths, func(i, j int) bool { return len(n.paths[i].prefix) > len(n.paths[j].prefix) })
	return n
}

func (n *normalizer) idToken(raw string) string {
	ord, ok := n.ids[raw]
	if !ok {
		ord = len(n.ids) + 1
		n.ids[raw] = ord
	}
	return "{{id:" + strconv.Itoa(ord) + "}}"
}

// pathBoundaryChars are the characters allowed around a path match
// (aligned with dsh's PATH_TEXT_BOUNDARY_RE, backtick included — model
// outputs wrap paths in markdown code spans).
const pathBoundaryChars = " \t\r\n<>'\"`()[]{},;:!?="

// isPathBoundary reports whether the match of prefix at s[start:end]
// stands on its own (not the head of a longer sibling name).
func isPathBoundary(s string, start, end int) bool {
	if start > 0 && !strings.ContainsRune(pathBoundaryChars, rune(s[start-1])) && s[start-1] != '/' {
		return false
	}
	if end < len(s) && !strings.ContainsRune(pathBoundaryChars, rune(s[end])) && s[end] != '/' && s[end] != '.' {
		return false
	}
	return true
}

// minVolatileMatchLen is the shortest string any volatile-value matcher
// below can accept: ctxm_+24 hex = 29. Shorter strings (the vast
// majority — numbers, short labels, fixed enums) skip the regex work
// entirely.
const minVolatileMatchLen = len("ctxm_") + 24

func (n *normalizer) scrubString(s string) string {
	for _, p := range n.paths {
		cursor := 0
		for cursor <= len(s) {
			idx := strings.Index(s[cursor:], p.prefix)
			if idx < 0 {
				break
			}
			idx += cursor
			if !isPathBoundary(s, idx, idx+len(p.prefix)) {
				// Not a standalone path here; keep scanning past it — a later
				// occurrence may still be replaceable.
				cursor = idx + len(p.prefix)
				continue
			}
			s = s[:idx] + p.token + s[idx+len(p.prefix):]
			cursor = idx + len(p.token)
		}
	}
	if len(s) >= minVolatileMatchLen {
		if loomIDRe.MatchString(s) {
			return n.idToken(s)
		}
		if hexVolatileRe.MatchString(s) {
			return n.hashToken(s)
		}
		// IDs embedded inside larger text (tool previews, conclusions)
		// get the same ordinal tokens as their field-valued occurrences.
		// Every ID form carries an underscore, so the full-string scan
		// is skipped for plaintext without one.
		if strings.Contains(s, "_") {
			s = loomIDEmbeddedRe.ReplaceAllStringFunc(s, n.idToken)
		}
	}
	// Run-volatile text shared with the request fingerprint: the system
	// prompt's current-date / platform-shell lines and the skills
	// catalog. UNCONDITIONAL — its date line alone ("Current date:
	// 2026-08-15 UTC", 28 chars, no underscore) is shorter than
	// minVolatileMatchLen, so the ID/hash fast paths must not skip it.
	return replay.ScrubVolatileText(s)
}

func (n *normalizer) hashToken(raw string) string {
	ord, ok := n.hashes[raw]
	if !ok {
		ord = len(n.hashes) + 1
		n.hashes[raw] = ord
	}
	return "{{hash:" + strconv.Itoa(ord) + "}}"
}

// normalize walks a decoded JSON value, applying the volatile-value
// rewrites. Keys under timeKeys zero their value. zeroSequence marks the
// broker-global RuntimeEvent sequence (non-deterministic under
// concurrent sub-agents); per-session sequences (durable events,
// messages) stay — they are deterministic.
func (n *normalizer) normalize(v any, zeroSequence bool) any {
	switch node := v.(type) {
	case map[string]any:
		out := make(map[string]any, len(node))
		// Iterate in sorted key order: Go map order is random, and the
		// first-appearance ordinals of ID/hash tokens would otherwise
		// reshuffle between runs.
		keys := make([]string, 0, len(node))
		for key := range node {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for _, key := range keys {
			child := node[key]
			switch {
			case key == "duration_ms":
				// Thinking-block wall time is measured per run and the
				// field is omitempty: zeroing keeps the key present, so a
				// block timed at 0ms (key absent) and one timed at >=1ms
				// (key zeroed) would diff spuriously. Drop the key
				// outright so presence can never flake the golden.
				continue
			case timeKeys[key]:
				out[key] = 0
			case zeroSequence && key == "sequence":
				out[key] = 0
			default:
				out[key] = n.normalize(child, false)
			}
		}
		return out
	case []any:
		out := make([]any, len(node))
		for i, child := range node {
			out[i] = n.normalize(child, false)
		}
		return out
	case string:
		return n.scrubString(node)
	default:
		return node
	}
}

// normalizeJSON round-trips one JSON-marshalable value through the
// normalizer and returns the canonical compact encoding (map keys are
// sorted by encoding/json, so the output is deterministic). A coding
// failure is reported as a visible sentinel line, not an empty string —
// an empty golden line would send a diff hunt in entirely the wrong
// direction.
func (n *normalizer) normalizeJSON(v any, zeroSequence bool) string {
	data, err := json.Marshal(v)
	if err != nil {
		return normalizeErrorSentinel("marshal input", err)
	}
	var decoded any
	if err := json.Unmarshal(data, &decoded); err != nil {
		return normalizeErrorSentinel("unmarshal input", err)
	}
	out, err := json.Marshal(n.normalize(decoded, zeroSequence))
	if err != nil {
		return normalizeErrorSentinel("marshal normalized", err)
	}
	return string(out)
}

// normalizeErrorSentinel is the in-band failure marker embedded in a
// golden when a value cannot be round-tripped. It keeps the golden
// diffable while making the failure unmistakable.
func normalizeErrorSentinel(stage string, err error) string {
	msg := strings.ReplaceAll(err.Error(), `"`, `'`)
	return `{"normalize_error":"` + stage + `: ` + msg + `"}`
}

// NormalizeEvents renders the broker's DURABLE event stream as
// normalized JSONL — one compact JSON object per line, in publish order.
// Ephemeral deltas (model.text_delta and friends) are excluded by
// contract: their fragmentation is a stream artifact already pinned
// byte-for-byte by the calls.jsonl fixture, and their embedded path
// fragments can straddle delta boundaries where no normalizer can see
// them whole.
func NormalizeEvents(ctx NormalizeContext, events []runtimeevent.RuntimeEvent) string {
	n := newNormalizer(ctx)
	var sb strings.Builder
	for _, evt := range events {
		if !evt.Durable {
			continue
		}
		sb.WriteString(n.normalizeJSON(evt, true))
		sb.WriteByte('\n')
	}
	return sb.String()
}

// NormalizeTranscripts renders the persisted transcripts (one
// SessionInspection per session, root first) as normalized JSON. The
// durable Events of the inspection are included: they are the persisted
// half of the golden pair.
func NormalizeTranscripts(ctx NormalizeContext, inspections []domain.SessionInspection) string {
	n := newNormalizer(ctx)
	views := make([]map[string]any, 0, len(inspections))
	for i, insp := range inspections {
		var decoded map[string]any
		data, err := json.Marshal(insp)
		if err != nil {
			// A session that cannot round-trip must not silently vanish
			// from the golden — emit an in-band sentinel in its place.
			views = append(views, map[string]any{
				"normalize_error": "marshal inspection " + strconv.Itoa(i) + ": " + err.Error(),
			})
			continue
		}
		if err := json.Unmarshal(data, &decoded); err != nil {
			views = append(views, map[string]any{
				"normalize_error": "unmarshal inspection " + strconv.Itoa(i) + ": " + err.Error(),
			})
			continue
		}
		views = append(views, map[string]any{
			"session":    n.normalize(decoded["session"], false),
			"transcript": n.normalize(decoded["transcript"], false),
			"events":     n.normalize(decoded["events"], false),
		})
	}
	out, err := json.MarshalIndent(views, "", "  ")
	if err != nil {
		return normalizeErrorSentinel("marshal views", err) + "\n"
	}
	return string(out) + "\n"
}
