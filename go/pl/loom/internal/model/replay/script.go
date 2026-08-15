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
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// calls.jsonl line types (REPLAY_TESTING_DESIGN §3.2). Every recorded
// model call is a `call_start` line (carrying the full request and its
// fingerprint), followed by one `event` line per streamed ModelEvent,
// closed by exactly one terminator: `call_end` (clean EOF), `call_error`
// (the stream failed), or `call_hang` (the stream was cancelled while
// blocked — the steering/cancel fixture shape).
const (
	lineCallStart = "call_start"
	lineEvent     = "event"
	lineCallEnd   = "call_end"
	lineCallError = "call_error"
	lineCallHang  = "call_hang"

	// hangUntilCancelled is the only hang shape loom records: the stream
	// blocked until its context was cancelled.
	hangUntilCancelled = "cancelled"
)

// CallsFileName is the primary (root session) recording file name.
const CallsFileName = "calls.jsonl"

// callsFilePrefix/callsFileSuffix delimit per-subsession recording files:
// calls.<parent tool call ID>.jsonl.
const (
	callsFilePrefix = "calls."
	callsFileSuffix = ".jsonl"
)

// CallsFileFor maps a binding key to its recording file name.
func CallsFileFor(bindingKey string) string {
	if bindingKey == RootBindingKey {
		return CallsFileName
	}
	return callsFilePrefix + bindingKey + callsFileSuffix
}

// BindingKeyForFile maps a recording file name back to its binding key;
// ok is false for files that are not recordings.
func BindingKeyForFile(name string) (key string, ok bool) {
	if name == CallsFileName {
		return RootBindingKey, true
	}
	if strings.HasPrefix(name, callsFilePrefix) && strings.HasSuffix(name, callsFileSuffix) {
		key = strings.TrimSuffix(strings.TrimPrefix(name, callsFilePrefix), callsFileSuffix)
		return key, key != ""
	}
	return "", false
}

// CallOutcome classifies how a recorded call terminated.
type CallOutcome string

const (
	// OutcomeEnd is a cleanly exhausted stream (Recv -> io.EOF).
	OutcomeEnd CallOutcome = "end"
	// OutcomeError is a stream that failed with a non-cancellation error;
	// the error text is replayed after the recorded prefix events.
	OutcomeError CallOutcome = "error"
	// OutcomeHang is a stream that blocked until its context was
	// cancelled; replay blocks at the same position until cancellation.
	OutcomeHang CallOutcome = "hang"
)

// Call is one recorded model call: the request that issued it, the full
// fidelity ModelEvent sequence it streamed, and how it terminated.
type Call struct {
	Seq         int
	Fingerprint string
	Request     json.RawMessage
	Events      []domain.ModelEvent
	Outcome     CallOutcome
	ErrText     string
}

// Script is the ordered recording of one session's model calls.
type Script struct {
	Calls []Call
}

type lineEnvelope struct {
	Type string `json:"type"`
}

type callStartLine struct {
	Type        string          `json:"type"`
	Seq         int             `json:"seq"`
	Fingerprint string          `json:"fingerprint"`
	Request     json.RawMessage `json:"request"`
}

// eventLine flattens the ModelEvent next to the line type:
// {"type":"event","kind":"reasoning_delta","reasoning_delta":"..."}.
type eventLine struct {
	Type string `json:"type"`
	domain.ModelEvent
}

type callEndLine struct {
	Type string `json:"type"`
	Seq  int    `json:"seq"`
}

type callErrorLine struct {
	Type  string `json:"type"`
	Seq   int    `json:"seq"`
	Error string `json:"error"`
}

type callHangLine struct {
	Type  string `json:"type"`
	Seq   int    `json:"seq"`
	Until string `json:"until"`
}

// LoadScript parses a calls.jsonl recording, validating the line
// grammar strictly: a recording with a truncated or reordered tail fails
// loud here rather than replaying something subtly wrong. Fixture path
// tokens are detokenized with the live run's roots so recorded tool
// calls (whose arguments carry the record run's absolute paths) execute
// against the replay run's workspace.
func LoadScript(path string, scrubs []ScrubPath) (*Script, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("replay: open recording: %w", err)
	}
	defer f.Close()

	var script Script
	var open *Call // the call currently collecting events, nil between calls
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 0, 1<<20), 64<<20)
	lineNo := 0
	for scanner.Scan() {
		lineNo++
		text := strings.TrimSpace(scanner.Text())
		if text == "" {
			continue
		}
		fail := func(format string, args ...any) (*Script, error) {
			return nil, fmt.Errorf("replay: %s:%d: %s", path, lineNo, fmt.Sprintf(format, args...))
		}
		text = Detokenize(text, scrubs)
		var env lineEnvelope
		if err := json.Unmarshal([]byte(text), &env); err != nil {
			return fail("invalid JSON: %v", err)
		}
		switch env.Type {
		case lineCallStart:
			if open != nil {
				return fail("call_start while call #%d is still open", open.Seq)
			}
			var line callStartLine
			if err := json.Unmarshal([]byte(text), &line); err != nil {
				return fail("invalid call_start: %v", err)
			}
			if line.Seq != len(script.Calls)+1 {
				return fail("call_start seq = %d, want %d", line.Seq, len(script.Calls)+1)
			}
			open = &Call{Seq: line.Seq, Fingerprint: line.Fingerprint, Request: line.Request}
		case lineEvent:
			if open == nil {
				return fail("event outside any call")
			}
			var line eventLine
			if err := json.Unmarshal([]byte(text), &line); err != nil {
				return fail("invalid event: %v", err)
			}
			open.Events = append(open.Events, line.ModelEvent)
		case lineCallEnd, lineCallError, lineCallHang:
			if open == nil {
				return fail("%s without an open call", env.Type)
			}
			var seqLine callEndLine
			if err := json.Unmarshal([]byte(text), &seqLine); err != nil {
				return fail("invalid %s: %v", env.Type, err)
			}
			if seqLine.Seq != open.Seq {
				return fail("%s seq = %d, open call is #%d", env.Type, seqLine.Seq, open.Seq)
			}
			switch env.Type {
			case lineCallEnd:
				open.Outcome = OutcomeEnd
			case lineCallError:
				var line callErrorLine
				if err := json.Unmarshal([]byte(text), &line); err != nil {
					return fail("invalid call_error: %v", err)
				}
				open.Outcome = OutcomeError
				open.ErrText = line.Error
			case lineCallHang:
				var line callHangLine
				if err := json.Unmarshal([]byte(text), &line); err != nil {
					return fail("invalid call_hang: %v", err)
				}
				if line.Until != hangUntilCancelled {
					return fail("call_hang until = %q, want %q", line.Until, hangUntilCancelled)
				}
				open.Outcome = OutcomeHang
			}
			script.Calls = append(script.Calls, *open)
			open = nil
		default:
			return fail("unknown line type %q", env.Type)
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("replay: read %s: %w", path, err)
	}
	if open != nil {
		return nil, fmt.Errorf("replay: %s: call #%d has no terminator (truncated recording)", path, open.Seq)
	}
	return &script, nil
}

// LoadScripts loads every recording under dir: calls.jsonl binds to the
// root session, calls.<key>.jsonl to a delegated session. A missing root
// recording is an error — replay needs at least the primary session.
func LoadScripts(dir string, scrubs []ScrubPath) (map[string]*Script, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, fmt.Errorf("replay: read scenario dir: %w", err)
	}
	scripts := make(map[string]*Script)
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		key, ok := BindingKeyForFile(entry.Name())
		if !ok {
			continue
		}
		script, err := LoadScript(filepath.Join(dir, entry.Name()), scrubs)
		if err != nil {
			return nil, err
		}
		scripts[key] = script
	}
	if _, ok := scripts[RootBindingKey]; !ok {
		return nil, fmt.Errorf("replay: no %s under %s — record the scenario first (LOOM_SNAPSHOT=record)", CallsFileName, dir)
	}
	return scripts, nil
}
