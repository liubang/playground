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

// maze.go — the execution-trace maze: folds a session's event log into a
// "main path + detour branches" timeline (MazeData) rendered by the webui's
// trace tab, two-session compare view, and shared-session page.
//
// Data contract (mirrored by src/components/maze):
//   - one step = one model request (model.request_started →
//     model.response_completed); parallel tool calls within a step carry
//     their real spans (ToolResult.StartedAt/FinishedAt);
//   - the main path holds steps judged ok/answer; error/deadend/retry steps
//     become detours whose attach points at the nearest main-path step;
//   - sub-agent sessions fold into one aggregated detour node each
//     (sub=true), sharing the parent's clock;
//   - verdicts rest on the structured ToolResult.Status first; text
//     sniffing only detects "succeeded but empty" dead ends. Blind-retry
//     marking is behavioral (a consecutive same-tool cluster with similar
//     arguments and at least one failure), after AgentLens's deterministic
//     waste detection and dsh-trace-compare's calibration.

package app

import (
	"context"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// --- wire types (mirrored by protocol/types.ts Maze* interfaces) ---

// MazeData is the trace-maze payload: one lane per session on a shared
// wall-clock axis (seconds since the lane's first user message).
type MazeData struct {
	Tmax  float64    `json:"tmax"`
	Lanes []MazeLane `json:"lanes"`
}

// MazeLane is one session's maze: main path + detour branches + stats.
type MazeLane struct {
	Key       string     `json:"key"`
	SessionID string     `json:"session_id"`
	Title     string     `json:"title,omitempty"`
	Model     string     `json:"model,omitempty"`
	Main      []MazeNode `json:"main"`
	Detours   []MazeNode `json:"detours"`
	Stats     MazeStats  `json:"stats"`
}

// MazeStats aggregates one lane for the header strip.
type MazeStats struct {
	Steps   int     `json:"steps"`
	Tools   int     `json:"tools"`
	Rz      int     `json:"rz"`
	InTok   int64   `json:"in_tok"`
	RzTok   int64   `json:"rz_tok"`
	OutTok  int64   `json:"out_tok"`
	T       float64 `json:"t"`
	Main    int     `json:"main"`
	Detours int     `json:"detours"`
}

// MazeNode is one step (a model request plus its tool calls), or an
// aggregated sub-agent branch (Sub=true).
type MazeNode struct {
	Step  int        `json:"step"`
	Turn  int        `json:"turn"`
	S     float64    `json:"s"`
	E     float64    `json:"e"`
	Tools []MazeTool `json:"tools"`
	// Rz counts reasoning blocks; RzTxt is a flattened excerpt (≤2000
	// chars) for the detail panel.
	Rz    int    `json:"rz"`
	RzTxt string `json:"rz_txt,omitempty"`
	// Per-request token truth from the persisted response_completed
	// payload; nil for sessions written before per-request usage existed.
	InTok  *int64 `json:"in_tok,omitempty"`
	RzTok  *int64 `json:"rz_tok,omitempty"`
	OutTok *int64 `json:"out_tok,omitempty"`
	// V: ok | answer | error | deadend | retry | pending (unsettled tools).
	// Why carries user-facing UI copy (the webui is Chinese-first).
	V   string `json:"v"`
	Why string `json:"why,omitempty"`
	// Sub marks an aggregated sub-agent child node; Label is its title.
	Sub   bool   `json:"sub,omitempty"`
	Label string `json:"label,omitempty"`
	// Attach is the main-path step this detour hangs off (0 = origin).
	Attach int `json:"attach,omitempty"`
	// MsgSeq anchors the chat jump to the assistant message sequence.
	MsgSeq int64 `json:"msg_seq,omitempty"`
	// Retries counts model.request_retrying waits the step slept through.
	Retries int  `json:"retries,omitempty"`
	Live    bool `json:"live,omitempty"`
}

// MazeTool is one tool call inside a step.
type MazeTool struct {
	Name     string   `json:"name"`
	Args     string   `json:"args"`                // bar label (≤120 chars)
	ArgsFull string   `json:"args_full,omitempty"` // detail panel (≤4000 chars)
	S        float64  `json:"s"`
	E        *float64 `json:"e"` // null = still executing
	Dur      float64  `json:"dur"`
	Res      string   `json:"res"`                // hover excerpt (≤380 chars)
	ResFull  string   `json:"res_full,omitempty"` // detail panel (≤5000 chars)
	V        string   `json:"v"`                  // ok | error | deadend | retry | pending
	Why      string   `json:"why,omitempty"`      // user-facing UI copy
	CallID   string   `json:"call_id"`
	Status   string   `json:"status,omitempty"`
	ChildID  string   `json:"child_id,omitempty"` // sub-session spawned by delegate_task
}

// --- verdict rules ---

// Write tools: success confirmations are naturally short — no error means
// success; output length never implies a dead end.
var mazeWriteTools = map[string]bool{
	"write": true, "edit": true,
}

// Search tools: only an empty result counts as a dead end; any hit (even a
// single line) is success.
var mazeSearchTools = map[string]bool{
	"read_file": true, "grep": true, "glob": true, "list_dir": true,
	"web_search": true, "web_fetch": true, "kb_search": true, "kb_read": true,
	"git_log": true, "git_blame": true, "git_diff": true, "git_status": true,
	"git_merge_base": true, "view_image": true, "lint": true, "read_skill": true,
}

// Empty/no-hit markers (head window only): a genuine empty-result notice is
// the whole short message; "not found" quoted inside a file's content must
// not count.
var mazeNoResultNeedles = []string{"no matches", "no results", "not found in", "0 matches"}

const (
	mazeNoResultScanChars = 300
	// Blind retry: Jaccard similarity threshold over argument tokens of
	// adjacent same-tool calls / minimum cluster length.
	mazeRetrySimilarity  = 0.6
	mazeRetryMinCluster  = 2
	mazeTooltipResChars  = 380
	mazeDetailResChars   = 5000
	mazeArgsLabelChars   = 120
	mazeArgsDetailChars  = 4000
	mazeReasoningExcerpt = 2000
	// Step ids of aggregated sub-agent nodes start here so they never
	// collide with parent step ids.
	mazeChildStepBase = 100000
	// Defensive cap on sub-agent branches folded into one lane.
	mazeMaxChildren = 20
)

// --- event payload DTOs (mirror the agent package's unexported payloads,
// same pattern as controller.go's local DTOs) ---

type mazeRequestStartedDTO struct {
	RequestID domain.EventID `json:"request_id"`
	ModelName string         `json:"model_name"`
	Turn      int            `json:"turn,omitempty"`
}

type mazeRequestFailedDTO struct {
	RequestID domain.EventID `json:"request_id"`
	Stage     string         `json:"stage"`
	Code      string         `json:"code"`
	Message   string         `json:"message,omitempty"`
}

type mazeExecStartedDTO struct {
	CallID domain.ToolCallID `json:"call_id"`
}

type mazeExecCompletedDTO struct {
	CallID       domain.ToolCallID `json:"call_id"`
	Status       domain.ToolStatus `json:"status"`
	ErrorCode    string            `json:"error_code,omitempty"`
	ErrorMessage string            `json:"error_message,omitempty"`
	StartedAt    time.Time         `json:"started_at"`
	FinishedAt   time.Time         `json:"finished_at"`
}

// --- builder internals ---

// mazeStep accumulates one model request's maze state during the scan.
type mazeStep struct {
	node      MazeNode
	tools     map[domain.ToolCallID]*MazeTool
	toolOrder []domain.ToolCallID
	failed    bool
	// answered marks that the response (or a terminal failure) arrived;
	// an unanswered step is the in-flight one and stays on the main path.
	answered bool
}

// mazeChild is one resolved sub-agent child session folded into its
// parent's maze as an aggregated detour node.
type mazeChild struct {
	Label   string
	Running bool
	Events  []domain.Event
}

// mazeClock maps wall-clock time onto the lane axis (seconds, 0.1s grid).
// live reports whether the session is actively executing right now: only
// then do in-flight steps/tools extend to now — a crashed or finished
// session's unsettled work must not smear across the axis.
type mazeClock struct {
	anchor time.Time
	now    time.Time
	live   bool
}

func (c mazeClock) rel(t time.Time) float64 {
	s := t.Sub(c.anchor).Seconds()
	if s < 0 {
		s = 0
	}
	return float64(int(s*10+0.5)) / 10
}

func contentText(parts []domain.ContentPart) string {
	var b strings.Builder
	for _, p := range parts {
		if p.Kind == domain.PartText {
			b.WriteString(p.Text)
		}
	}
	return b.String()
}

func flattenWs(s string) string {
	return strings.Join(strings.Fields(s), " ")
}

// argSummary picks the most telling argument as the bar label, mirroring
// the tools' actual schemas (command / path / pattern / query / url).
func argSummary(raw json.RawMessage) string {
	if len(raw) == 0 {
		return ""
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		return truncateRunes(string(raw), mazeArgsLabelChars)
	}
	for _, k := range []string{"command", "path", "file_path", "pattern", "query", "url", "task"} {
		if v, ok := m[k].(string); ok && v != "" {
			if k == "pattern" {
				if p, _ := m["path"].(string); p != "" {
					return truncateRunes("pattern="+v+" path="+p, mazeArgsLabelChars)
				}
			}
			return truncateRunes(v, mazeArgsLabelChars)
		}
	}
	compact, err := json.Marshal(m)
	if err != nil {
		return truncateRunes(string(raw), mazeArgsLabelChars)
	}
	return truncateRunes(string(compact), mazeArgsLabelChars)
}

// scanSteps folds one session's events into verdict-settled steps on the
// given clock. Pending (in-flight) tools keep E=nil and do not vote.
func scanSteps(events []domain.Event, clk mazeClock) []*mazeStep {
	var steps []*mazeStep
	byRequest := make(map[domain.EventID]*mazeStep)
	byCall := make(map[domain.ToolCallID]*MazeTool)
	turnCount := 0

	for _, evt := range events {
		switch evt.Type {
		case domain.EventUserMessageAdded:
			turnCount++
		case domain.EventModelRequestStarted:
			var p mazeRequestStartedDTO
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			turn := p.Turn
			if turn <= 0 {
				// Logs written before turn was persisted: fall back to
				// counting user messages.
				turn = turnCount
				if turn <= 0 {
					turn = 1
				}
			}
			st := &mazeStep{
				node: MazeNode{
					Step: len(steps) + 1, Turn: turn,
					S: clk.rel(evt.Timestamp), E: clk.rel(evt.Timestamp),
				},
				tools: make(map[domain.ToolCallID]*MazeTool),
			}
			steps = append(steps, st)
			if !p.RequestID.IsZero() {
				byRequest[p.RequestID] = st
			}
		case domain.EventModelResponseCompleted:
			var p domain.ResponseCompletedPayload
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			reqID, _ := domain.ParseEventID(p.Message.Metadata["request_id"])
			st := byRequest[reqID]
			if st == nil && len(steps) > 0 {
				st = steps[len(steps)-1] // legacy fallback: nearest step
			}
			if st == nil {
				continue
			}
			st.answered = true
			st.node.E = clk.rel(evt.Timestamp)
			st.node.MsgSeq = p.Message.Sequence
			var rzTxt string
			for _, part := range p.Message.Parts {
				switch part.Kind {
				case domain.PartReasoning:
					if part.Reasoning != nil && part.Reasoning.Text != "" {
						st.node.Rz++
						rzTxt += part.Reasoning.Text
					}
				case domain.PartToolCall:
					if part.ToolCall == nil {
						continue
					}
					call := part.ToolCall
					tool := &MazeTool{
						Name:   call.Name,
						Args:   argSummary(call.Arguments),
						S:      st.node.S,
						CallID: call.ID.String(),
						V:      "pending",
					}
					if len(call.Arguments) > 0 {
						tool.ArgsFull = truncateRunes(flattenWs(string(call.Arguments)), mazeArgsDetailChars)
					}
					st.tools[call.ID] = tool
					st.toolOrder = append(st.toolOrder, call.ID)
					byCall[call.ID] = tool
				}
			}
			st.node.RzTxt = truncateRunes(flattenWs(rzTxt), mazeReasoningExcerpt)
			if u := p.Usage; u != nil {
				st.node.InTok = &u.InputTokens
				st.node.OutTok = &u.OutputTokens
				if u.ReasoningTokens > 0 {
					st.node.RzTok = &u.ReasoningTokens
				}
			}
		case domain.EventModelRequestFailed:
			var p mazeRequestFailedDTO
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			if st := byRequest[p.RequestID]; st != nil {
				st.failed = true
				st.answered = true
				st.node.E = clk.rel(evt.Timestamp)
				st.node.Why = "模型请求失败（" + p.Stage + "）: " + p.Message
			}
		case domain.EventModelRequestRetrying:
			var p mazeRequestFailedDTO // first fields share the same shape
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			if st := byRequest[p.RequestID]; st != nil {
				st.node.Retries++
			}
		case domain.EventToolExecutionStarted:
			var p mazeExecStartedDTO
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			if t := byCall[p.CallID]; t != nil {
				t.S = clk.rel(evt.Timestamp)
			}
		case domain.EventToolExecutionCompleted:
			var p mazeExecCompletedDTO
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			t := byCall[p.CallID]
			if t == nil {
				continue
			}
			t.Status = string(p.Status)
			if !p.StartedAt.IsZero() {
				t.S = clk.rel(p.StartedAt)
			}
			e := clk.rel(evt.Timestamp)
			if !p.FinishedAt.IsZero() {
				e = clk.rel(p.FinishedAt)
			}
			t.E = &e
			if p.Status == domain.ToolStatusError && p.ErrorMessage != "" {
				t.Why = p.ErrorMessage
			}
		case domain.EventToolResultAdded:
			var p domain.MessageEventPayload
			if err := json.Unmarshal(evt.Payload, &p); err != nil {
				continue
			}
			for _, part := range p.Message.Parts {
				if part.Kind != domain.PartToolResult || part.ToolResult == nil {
					continue
				}
				res := part.ToolResult
				t := byCall[res.CallID]
				if t == nil {
					continue
				}
				full := flattenWs(contentText(res.Content))
				t.ResFull = truncateRunes(full, mazeDetailResChars)
				t.Res = truncateRunes(full, mazeTooltipResChars)
				t.Status = string(res.Status)
				if !res.StartedAt.IsZero() {
					t.S = clk.rel(res.StartedAt)
				}
				if !res.FinishedAt.IsZero() {
					e := clk.rel(res.FinishedAt)
					t.E = &e
				}
				if res.Error != nil && res.Error.Message != "" {
					t.Why = res.Error.Message
				}
				if cid := res.Metadata["child_session_id"]; cid != "" {
					t.ChildID = cid
				}
				// A landed result settles the call: StartedAt/FinishedAt are
				// the precise span; fall back to the event time (legacy logs,
				// truncated replays) so the call never stays in-flight forever.
				if t.E == nil {
					e := clk.rel(evt.Timestamp)
					t.E = &e
				}
			}
		}
	}

	for _, st := range steps {
		for _, id := range st.toolOrder {
			st.node.Tools = append(st.node.Tools, *st.tools[id])
		}
		if st.node.Tools == nil {
			st.node.Tools = []MazeTool{} // wire shape: [], not null
		}
		for i := range st.node.Tools {
			t := &st.node.Tools[i]
			if t.E != nil {
				t.Dur = float64(int((*t.E-t.S)*10+0.5)) / 10
				if *t.E > st.node.E {
					st.node.E = *t.E
				}
			}
		}
		if st.failed {
			st.node.V = "error"
			continue
		}
		hasPending := false
		for i := range st.node.Tools {
			if st.node.Tools[i].E == nil {
				hasPending = true
				break
			}
		}
		// In-flight step: the request is out (no response yet) or tools are
		// still executing. While the session is live, the axis extends to
		// now and the step renders as a growing capsule; it does not vote.
		// In a dead session the step keeps its last known end instead.
		if clk.live && (!st.answered || hasPending) {
			st.node.Live = true
			st.node.V = "ok"
			st.node.E = clk.rel(clk.now)
		}
	}
	return steps
}

// settleVerdicts computes tool verdicts (structured status first, empty-
// result sniffing only for successful searches), marks blind-retry
// clusters, then aggregates step verdicts. Steps with in-flight tools
// stay on the main path until their outcomes land.
func settleVerdicts(steps []*mazeStep) {
	var settled []*MazeTool
	for _, st := range steps {
		for i := range st.node.Tools {
			t := &st.node.Tools[i]
			if t.E == nil {
				continue // in-flight calls do not vote
			}
			t.V, t.Why = toolVerdict(t)
			settled = append(settled, t)
		}
	}
	markRetryClusters(settled)
	for _, st := range steps {
		if st.failed || st.node.Live {
			continue // failed / in-flight steps already carry their verdict
		}
		if !st.answered {
			// Dead session, response never landed: neutral pending — never
			// mislabel it as an answer node.
			st.node.V = "pending"
			st.node.Why = "响应未到达（会话已中断）"
			continue
		}
		if len(st.node.Tools) == 0 {
			st.node.V = "answer"
			continue
		}
		worst := ""
		why := ""
		pending := false
		for i := range st.node.Tools {
			t := &st.node.Tools[i]
			if t.V == "pending" {
				pending = true
				continue
			}
			if mazeSeverity(t.V) > mazeSeverity(worst) {
				worst, why = t.V, t.Why
			}
		}
		switch {
		case worst != "":
			st.node.V = worst
			if st.node.Why == "" {
				st.node.Why = why
			}
		case pending:
			st.node.V = "pending"
		}
	}
}

func mazeSeverity(v string) int {
	switch v {
	case "error":
		return 4
	case "retry":
		return 3
	case "deadend":
		return 2
	case "ok", "answer":
		return 1
	}
	return 0
}

// toolVerdict judges one settled tool call. The structured Status is the
// primary source — text sniffing only detects the "succeeded but empty"
// dead end, which keeps the false-positive surface far below dsh's
// head/tail-window regexes on our own corpus.
func toolVerdict(t *MazeTool) (string, string) {
	switch domain.ToolStatus(t.Status) {
	case domain.ToolStatusError:
		if t.Why != "" {
			return "error", "执行失败: " + t.Why
		}
		return "error", "执行失败"
	case domain.ToolStatusTimeout:
		return "error", "执行超时"
	case domain.ToolStatusCancelled:
		// A user cancel is a deliberate action, not a failed exploration:
		// it never counts as a detour.
		return "ok", "已取消"
	}
	head := t.ResFull
	if head == "" {
		head = t.Res
	}
	head = truncateRunes(head, mazeNoResultScanChars)
	if mazeWriteTools[t.Name] {
		return "ok", ""
	}
	noHit := strings.TrimSpace(head) == ""
	if !noHit {
		lower := strings.ToLower(head)
		for _, needle := range mazeNoResultNeedles {
			if strings.Contains(lower, needle) {
				noHit = true
				break
			}
		}
	}
	if mazeSearchTools[t.Name] {
		if noHit {
			return "deadend", "检索无命中"
		}
		return "ok", ""
	}
	if noHit {
		return "deadend", "无输出"
	}
	return "ok", ""
}

func argTokenSet(s string) map[string]struct{} {
	out := make(map[string]struct{})
	for _, w := range strings.FieldsFunc(s, func(r rune) bool {
		return !(r == '.' || r == '-' || r == '_' || r == '/' ||
			(r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') ||
			(r >= 0x4e00 && r <= 0x9fff))
	}) {
		if len([]rune(w)) > 2 {
			out[w] = struct{}{}
		}
	}
	return out
}

func argSimilarity(a, b string) float64 {
	ta, tb := argTokenSet(a), argTokenSet(b)
	if len(ta) == 0 || len(tb) == 0 {
		return 0
	}
	inter := 0
	for w := range ta {
		if _, ok := tb[w]; ok {
			inter++
		}
	}
	return float64(inter) / float64(len(ta)+len(tb)-inter)
}

// markRetryClusters is the behavioral detector: a consecutive same-tool
// cluster with similar arguments AND at least one failure re-marks its
// non-failed members as blind retries — without the failure constraint,
// legitimate runs like "edit the same file repeatedly" would be framed.
func markRetryClusters(calls []*MazeTool) {
	start := 0
	for i := 1; i <= len(calls); i++ {
		brk := i == len(calls) ||
			calls[i].Name != calls[i-1].Name ||
			argSimilarity(calls[i].Args, calls[i-1].Args) < mazeRetrySimilarity
		if brk {
			if n := i - start; n >= mazeRetryMinCluster {
				cluster := calls[start:i]
				fails := 0
				for _, c := range cluster {
					if c.V == "error" {
						fails++
					}
				}
				if fails > 0 {
					for _, c := range cluster {
						if c.V != "error" {
							c.V = "retry"
							c.Why = "同一操作连续重试，簇内有失败，判为盲目重试"
						}
					}
				}
			}
			start = i
		}
	}
}

// buildLane scans events into a partitioned lane. children fold in as
// aggregated detour nodes on the same clock.
func buildLane(key string, sessionID domain.SessionID, title string, events []domain.Event, children []mazeChild, clk mazeClock) MazeLane {
	steps := scanSteps(events, clk)
	settleVerdicts(steps)

	model := ""
	for _, evt := range events {
		if evt.Type == domain.EventModelRequestStarted {
			var p mazeRequestStartedDTO
			if json.Unmarshal(evt.Payload, &p) == nil && p.ModelName != "" {
				model = p.ModelName
			}
		}
	}

	// Partition first: sub-agent detours anchor against the complete main
	// path. Both slices start non-nil so the wire shape is always an array,
	// never null.
	main := []MazeNode{}
	detours := []MazeNode{}
	lastMain := 0
	tmax := 0.1
	stats := MazeStats{}
	for _, st := range steps {
		n := st.node
		if n.E > tmax {
			tmax = n.E
		}
		stats.Steps++
		stats.Tools += len(n.Tools)
		stats.Rz += n.Rz
		if n.InTok != nil {
			stats.InTok += *n.InTok
		}
		if n.RzTok != nil {
			stats.RzTok += *n.RzTok
		}
		if n.OutTok != nil {
			stats.OutTok += *n.OutTok
		}
		if n.V == "ok" || n.V == "answer" || n.V == "pending" || n.Live {
			main = append(main, n)
			lastMain = n.Step
		} else {
			n.Attach = lastMain
			detours = append(detours, n)
		}
	}

	// Sub-agent detours hang off the step that spawned them (the nearest
	// main step by time).
	childEnd := 0.0
	for i, child := range children {
		node := childDetourNode(child, i, clk)
		if node == nil {
			continue
		}
		attach := 0
		turn := 1
		for _, m := range main {
			if m.S <= node.S {
				attach, turn = m.Step, m.Turn
			} else {
				break
			}
		}
		node.Attach = attach
		node.Turn = turn
		detours = append(detours, *node)
		if node.E > childEnd {
			childEnd = node.E
		}
	}
	if childEnd > tmax {
		tmax = childEnd
	}
	stats.T = tmax
	stats.Main = len(main)
	stats.Detours = len(detours)

	// Keep detours in chronological order when sub-agent branches mix with
	// plain ones.
	sort.SliceStable(detours, func(i, j int) bool { return detours[i].S < detours[j].S })

	return MazeLane{
		Key: key, SessionID: sessionID.String(), Title: title, Model: model,
		Main: main, Detours: detours, Stats: stats,
	}
}

// childDetourNode aggregates one sub-agent session into a single detour
// node: span = child activity span, sub-bars = its judged tool calls.
func childDetourNode(child mazeChild, index int, clk mazeClock) *MazeNode {
	steps := scanSteps(child.Events, mazeClock{anchor: clk.anchor, now: clk.now, live: child.Running})
	settleVerdicts(steps)
	if len(steps) == 0 {
		return nil
	}
	node := &MazeNode{
		Step:  mazeChildStepBase + index,
		Sub:   true,
		Label: child.Label,
		S:     steps[0].node.S,
		Live:  child.Running,
		Tools: []MazeTool{},
	}
	var settledErr bool
	rzTokSum, outTokSum, inTokSum := int64(0), int64(0), int64(0)
	hasTok := false
	var rzTxt string
	for _, st := range steps {
		n := st.node
		if n.E > node.E {
			node.E = n.E
		}
		node.Rz += n.Rz
		rzTxt += n.RzTxt + " "
		node.Tools = append(node.Tools, n.Tools...)
		if n.InTok != nil {
			inTokSum += *n.InTok
			hasTok = true
		}
		if n.RzTok != nil {
			rzTokSum += *n.RzTok
		}
		if n.OutTok != nil {
			outTokSum += *n.OutTok
		}
		if n.V == "error" {
			settledErr = true
		}
	}
	node.RzTxt = truncateRunes(flattenWs(rzTxt), mazeReasoningExcerpt)
	if hasTok {
		node.InTok = &inTokSum
		node.OutTok = &outTokSum
		if rzTokSum > 0 {
			node.RzTok = &rzTokSum
		}
	}
	switch {
	case child.Running:
		node.V = "ok"
		node.Why = "子代理仍在运行"
	case settledErr:
		node.V = "error"
		node.Why = "子代理支路含失败步骤"
	default:
		node.V = "ok"
		node.Why = "子代理任务已完成，结果汇回主干"
	}
	return node
}

// BuildMaze converts one session's event log into maze data. children are
// the session's resolved sub-agent child sessions (may be empty). running
// reports whether the session is actively executing in-process (it gates
// in-flight extension to now). A session with no activity yields one
// zero-stats lane so the wire shape stays stable.
//
// Wire contract: Main/Detours/Tools always serialize as JSON arrays, never
// null — the frontend iterates them without nil guards.
func BuildMaze(sessionID domain.SessionID, title string, events []domain.Event, children []mazeChild, now time.Time, running bool) *MazeData {
	if len(events) == 0 {
		return &MazeData{Tmax: 60, Lanes: []MazeLane{{
			Key: "l1", SessionID: sessionID.String(), Title: title,
			Main: []MazeNode{}, Detours: []MazeNode{}, Stats: MazeStats{},
		}}}
	}
	anchor := events[0].Timestamp
	for _, evt := range events {
		if evt.Type == domain.EventUserMessageAdded {
			anchor = evt.Timestamp
			break
		}
	}
	lane := buildLane("l1", sessionID, title, events, children, mazeClock{anchor: anchor, now: now, live: running})
	tmax := lane.Stats.T
	if tmax < 60 {
		tmax = 60
	}
	return &MazeData{Tmax: tmax, Lanes: []MazeLane{lane}}
}

// --- SessionService integration ---

// mazeChildrenFor resolves the session's sub-agent children: each
// delegate_task tool result carrying child_session_id gets its child
// session inspected and folded in (capped at mazeMaxChildren, one level
// deep — grandchildren appear in the child's own maze view).
func (s *SessionService) mazeChildrenFor(ctx context.Context, sqlite *session.SQLiteStore, events []domain.Event) []mazeChild {
	seen := make(map[domain.SessionID]bool)
	var children []mazeChild
	for _, evt := range events {
		if evt.Type != domain.EventToolResultAdded {
			continue
		}
		var p domain.MessageEventPayload
		if json.Unmarshal(evt.Payload, &p) != nil {
			continue
		}
		for _, part := range p.Message.Parts {
			if part.Kind != domain.PartToolResult || part.ToolResult == nil {
				continue
			}
			cid := part.ToolResult.Metadata["child_session_id"]
			if cid == "" {
				continue
			}
			childID, err := domain.ParseSessionID(cid)
			if err != nil || seen[childID] {
				continue
			}
			seen[childID] = true
			if len(children) >= mazeMaxChildren {
				return children
			}
			insp, err := sqlite.InspectSession(ctx, childID)
			if err != nil || len(insp.Events) == 0 {
				continue
			}
			children = append(children, mazeChild{
				Label:   childMazeLabel(ctx, sqlite, childID),
				Running: mazeChildRunning(insp.Events),
				Events:  insp.Events,
			})
		}
	}
	return children
}

// childMazeLabel summarizes the child session's first user message as the
// branch label (best-effort: falls back to the short session id).
func childMazeLabel(ctx context.Context, sqlite *session.SQLiteStore, id domain.SessionID) string {
	if titles, err := sqlite.FirstUserMessageTexts(ctx, []domain.SessionID{id}); err == nil {
		if t := summarizeSessionTitle(titles[id]); t != "" {
			return t
		}
	}
	short := id.String()
	if len(short) > 12 {
		short = short[:12]
	}
	return short
}

// mazeChildRunning reports whether the child session's latest run never
// reached a terminal event.
func mazeChildRunning(events []domain.Event) bool {
	for i := len(events) - 1; i >= 0; i-- {
		switch events[i].Type {
		case domain.EventRunCompleted, domain.EventRunFailed, domain.EventRunCancelled, domain.EventRunInterrupted:
			return false
		case domain.EventRunCreated:
			return true
		}
	}
	return false
}

// mazeForSession inspects the session and builds its maze, sub-agent
// children folded in. running gates in-flight extension: it is true only
// while the session's in-process controller is actively executing.
func (s *SessionService) mazeForSession(ctx context.Context, sqlite *session.SQLiteStore, id domain.SessionID) (*MazeData, error) {
	inspection, err := sqlite.InspectSession(ctx, id)
	if err != nil {
		return nil, err
	}
	title := ""
	if titles, err := sqlite.FirstUserMessageTexts(ctx, []domain.SessionID{id}); err == nil {
		title = summarizeSessionTitle(titles[id])
	}
	running := false
	if h, ok := s.Get(id); ok {
		switch h.Controller.State() {
		case ControllerStateRunning, ControllerStateCancelling, ControllerStateBooting:
			running = true
		}
	}
	children := s.mazeChildrenFor(ctx, sqlite, inspection.Events)
	return BuildMaze(id, title, inspection.Events, children, time.Now().UTC(), running), nil
}

func (s *SessionService) mazeStore() (*session.SQLiteStore, error) {
	sqlite, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return nil, fmt.Errorf("maze is unavailable for this store")
	}
	return sqlite, nil
}

// Maze builds the trace-maze payload for a session (authenticated path).
func (s *SessionService) Maze(ctx context.Context, id domain.SessionID) (*MazeData, error) {
	sqlite, err := s.mazeStore()
	if err != nil {
		return nil, err
	}
	return s.mazeForSession(ctx, sqlite, id)
}

// SharedMaze builds the trace-maze payload through a public share token.
// The data is a timing/verdict projection of the same transcript the share
// view already exposes — no privilege is widened.
func (s *SessionService) SharedMaze(ctx context.Context, token string) (*MazeData, error) {
	store, err := s.shareStore()
	if err != nil {
		return nil, err
	}
	sessionID, err := s.resolveShare(ctx, store, token)
	if err != nil {
		return nil, err
	}
	sqlite, err := s.mazeStore()
	if err != nil {
		return nil, err
	}
	return s.mazeForSession(ctx, sqlite, sessionID)
}
