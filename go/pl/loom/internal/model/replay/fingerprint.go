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

package replay

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// volatileKeys are JSON object keys dropped from the fingerprint
// projection: their values are minted per run (message/event/artifact
// IDs, timestamps) and would falsely mismatch every replay. Tool call
// IDs are NOT volatile — replayed assistant streams carry the recorded
// IDs, so they stay in the projection.
var volatileKeys = map[string]bool{
	"id":                  true,
	"created_at":          true,
	"updated_at":          true,
	"started_at":          true,
	"finished_at":         true,
	"timestamp":           true,
	"time":                true,
	"sequence":            true,
	"revision":            true,
	"part_index":          true,
	"artifact_id":         true,
	"session_id":          true,
	"run_id":              true,
	"message_id":          true,
	"event_id":            true,
	"checkpoint_id":       true,
	"parent_tool_call_id": true,
}

// ScrubPath pairs an environment-dependent absolute path root (the
// record run's and the replay run's temp dirs differ) with the stable
// token that replaces it — in fingerprints AND inside the recorded
// fixture itself, so a committed calls.jsonl is portable (the dsh
// tokenizeSessionFixtureCwd equivalent). On replay the tokens are
// substituted back with the live roots before the events reach the loop.
type ScrubPath struct {
	Prefix string
	Token  string
}

// Fingerprint derives the request identity recorded in call_start and
// re-derived on replay (REPLAY_TESTING_DESIGN §4.3): a SHA-256 over the
// canonical JSON of the request header (model/reasoning/limits/tools)
// plus the full message content, with per-run-volatile fields stripped
// and environment-dependent path roots replaced by stable tokens.
//
// scrubPaths are the environment-dependent path roots (workspace, loom
// home, artifact dir) that legitimately differ between the record run
// and a replay run; each is replaced by its stable token before hashing,
// longest first so a nested root cannot corrupt a shorter one.
func Fingerprint(req domain.ModelRequest, scrubPaths []ScrubPath) string {
	data, err := json.Marshal(Projection(req, scrubPaths))
	if err != nil {
		return ""
	}
	sum := sha256.Sum256(data)
	return "sha256:" + hex.EncodeToString(sum[:])
}

// Projection is the fingerprint's canonical content view, exported so a
// drift warning can show WHAT changed, not just that something did.
func Projection(req domain.ModelRequest, scrubPaths []ScrubPath) map[string]any {
	projection := map[string]any{
		"model_name":  req.ModelName,
		"reasoning":   jsonRoundTrip(req.Reasoning),
		"max_tokens":  req.MaxTokens,
		"temperature": req.Temperature,
		"tools":       jsonRoundTrip(req.Tools),
		"messages":    messagesProjection(req.Messages),
	}
	if req.ResponseFormat != nil {
		projection["response_format"] = jsonRoundTrip(req.ResponseFormat)
	}
	stripVolatileKeys(projection)
	scrubProjectionPaths(projection, scrubPaths)
	return projection
}

// driftDetail pinpoints the first differing projection paths between the
// recorded request and the live one, so a fingerprint warning says WHAT
// drifted (the scrub tokens make the two sides comparable despite their
// different temp roots).
func driftDetail(call Call, live domain.ModelRequest, scrubPaths []ScrubPath) string {
	var recorded domain.ModelRequest
	if err := json.Unmarshal(call.Request, &recorded); err != nil {
		return ""
	}
	var diffs []string
	collectProjectionDiffs("", Projection(recorded, scrubPaths), Projection(live, scrubPaths), &diffs, 5)
	if len(diffs) == 0 {
		return ""
	}
	return "first differences (recorded vs live):\n  " + strings.Join(diffs, "\n  ")
}

func collectProjectionDiffs(path string, a, b any, out *[]string, limit int) {
	if len(*out) >= limit {
		return
	}
	switch an := a.(type) {
	case map[string]any:
		bn, ok := b.(map[string]any)
		if !ok {
			*out = append(*out, fmt.Sprintf("%s: recorded is an object, live is %T", path, b))
			return
		}
		keys := map[string]bool{}
		for k := range an {
			keys[k] = true
		}
		for k := range bn {
			keys[k] = true
		}
		sorted := make([]string, 0, len(keys))
		for k := range keys {
			sorted = append(sorted, k)
		}
		sort.Strings(sorted)
		for _, k := range sorted {
			collectProjectionDiffs(path+"."+k, an[k], bn[k], out, limit)
		}
	case []any:
		bn, ok := b.([]any)
		if !ok {
			*out = append(*out, fmt.Sprintf("%s: recorded is an array, live is %T", path, b))
			return
		}
		if len(an) != len(bn) {
			*out = append(*out, fmt.Sprintf("%s: recorded has %d element(s), live has %d", path, len(an), len(bn)))
			return
		}
		for i := range an {
			collectProjectionDiffs(fmt.Sprintf("%s[%d]", path, i), an[i], bn[i], out, limit)
		}
	default:
		if fmt.Sprintf("%v", a) != fmt.Sprintf("%v", b) {
			as, bs := fmt.Sprintf("%v", a), fmt.Sprintf("%v", b)
			if len(as) > 2000 {
				as = as[:2000] + "..."
			}
			if len(bs) > 2000 {
				bs = bs[:2000] + "..."
			}
			*out = append(*out, fmt.Sprintf("%s:\n    recorded: %s\n    live:     %s", path, as, bs))
		}
	}
}

// messagesProjection keeps role + parts per message; the message-level
// volatile fields (ID, sequence, timestamps) never enter the projection.
func messagesProjection(messages []domain.Message) []any {
	out := make([]any, 0, len(messages))
	for _, msg := range messages {
		out = append(out, map[string]any{
			"role":  string(msg.Role),
			"parts": jsonRoundTrip(msg.Parts),
		})
	}
	return out
}

// jsonRoundTrip converts a typed value into its plain JSON tree
// (map/slice/scalar) so the projection marshals canonically.
func jsonRoundTrip(v any) any {
	data, err := json.Marshal(v)
	if err != nil {
		return nil
	}
	var out any
	if err := json.Unmarshal(data, &out); err != nil {
		return nil
	}
	return out
}

func stripVolatileKeys(v any) {
	switch node := v.(type) {
	case map[string]any:
		for key, child := range node {
			if volatileKeys[key] {
				delete(node, key)
				continue
			}
			stripVolatileKeys(child)
		}
	case []any:
		for _, child := range node {
			stripVolatileKeys(child)
		}
	}
}

func scrubProjectionPaths(v any, scrubPaths []ScrubPath) {
	paths := append([]ScrubPath(nil), scrubPaths...)
	sort.Slice(paths, func(i, j int) bool { return len(paths[i].Prefix) > len(paths[j].Prefix) })
	var walk func(any)
	walk = func(node any) {
		switch n := node.(type) {
		case map[string]any:
			for key, child := range n {
				if s, ok := child.(string); ok {
					n[key] = scrubString(s, paths)
					continue
				}
				walk(child)
			}
		case []any:
			for i, child := range n {
				if s, ok := child.(string); ok {
					n[i] = scrubString(s, paths)
					continue
				}
				walk(child)
			}
		}
	}
	walk(v)
}

// loomIDTextRe matches loom-minted IDs embedded INSIDE larger strings
// (a sub-agent tool result renders {"child_session_id":"sess_..."} as
// text). They are re-minted every run, so the fingerprint replaces each
// match with a fixed token — positional consistency between the record
// and replay runs is what matters, not identity.
var loomIDTextRe = regexp.MustCompile(`(sess|run|turn|msg|tc|evt|art|ckpt|ws)_[0-9a-f]{32}|ctxm_[0-9a-f]{24}`)

func scrubString(s string, paths []ScrubPath) string {
	return ScrubVolatileText(loomIDTextRe.ReplaceAllString(ScrubPaths(s, paths), "{{id}}"))
}

// withPrivateAliases adds the macOS /private-prefixed spelling of every
// absolute root: t.TempDir() reports /var/... while filesystem
// canonicalization (EvalSymlinks inside the tools) reports
// /private/var/... — both spellings appear in one run's data.
func withPrivateAliases(paths []ScrubPath) []ScrubPath {
	out := append([]ScrubPath(nil), paths...)
	for _, p := range paths {
		if strings.HasPrefix(p.Prefix, "/") && !strings.HasPrefix(p.Prefix, "/private/") {
			out = append(out, ScrubPath{Prefix: "/private" + p.Prefix, Token: p.Token})
		}
	}
	return out
}

// ScrubPaths replaces every path prefix with its token (longest first).
// Applied to recorded fixtures at write time; Detokenize reverses it on
// replay. Plain substring replacement is safe on JSON text for the
// POSIX paths these fixtures carry (no quotes/backslashes to escape).
func ScrubPaths(s string, paths []ScrubPath) string {
	sorted := withPrivateAliases(paths)
	sort.Slice(sorted, func(i, j int) bool { return len(sorted[i].Prefix) > len(sorted[j].Prefix) })
	for _, p := range sorted {
		if p.Prefix == "" || p.Token == "" {
			continue
		}
		s = strings.ReplaceAll(s, p.Prefix, p.Token)
	}
	return s
}

// Detokenize reverses ScrubPaths: fixture tokens become the live run's
// path roots before recorded events reach the loop.
func Detokenize(s string, paths []ScrubPath) string {
	sorted := append([]ScrubPath(nil), paths...)
	sort.Slice(sorted, func(i, j int) bool { return len(sorted[i].Token) > len(sorted[j].Token) })
	for _, p := range sorted {
		if p.Prefix == "" || p.Token == "" {
			continue
		}
		s = strings.ReplaceAll(s, p.Token, p.Prefix)
	}
	return s
}

// currentDateRe matches the system prompt's dynamic date line
// ("Current date: 2026-08-15 UTC"): legitimate day-to-day drift, not a
// request change, so both the fingerprint and the snapshot normalizer
// tokenize it.
var currentDateRe = regexp.MustCompile(`Current date: \d{4}-\d{2}-\d{2} [A-Za-z]+`)

// platformShellRe matches the system prompt's platform/shell line: it
// reflects the HOST the test runs on (the bazel sandbox strips $SHELL;
// CI runners differ), never the code under test.
var platformShellRe = regexp.MustCompile(`Platform: [^\n,]+, Shell: [^\n]+`)

// skillsCatalogRe matches the system prompt's skills catalog section,
// from its heading through the catalog's fixed closing line ("When a
// skill is missing, say so briefly and continue with the best
// alternative."). The catalog lists whatever skill roots the RECORDING
// machine had installed — machine specific (and potentially private)
// content that must neither enter a fingerprint nor a committed
// fixture: a replay machine without those roots composes a
// catalog-free prompt, so both sides strip the section and stay
// comparable. The trailing blank line goes with it, keeping the
// surrounding sections' spacing intact.
var skillsCatalogRe = regexp.MustCompile(`(?s)# Available Skills\n.*?best alternative\.\n\n`)

// ScrubVolatileText rewrites run-volatile text that is NOT a request
// change: the system prompt's current-date and platform/shell lines,
// and the machine-local skills catalog section. Shared by the request
// fingerprint and the snapshot normalizer.
func ScrubVolatileText(s string) string {
	s = currentDateRe.ReplaceAllString(s, "Current date: {{date}}")
	s = platformShellRe.ReplaceAllString(s, "Platform: {{platform}}, Shell: {{shell}}")
	return skillsCatalogRe.ReplaceAllString(s, "")
}
