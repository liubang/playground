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

package server

import (
	"bufio"
	"bytes"
	"encoding/json"
	"net/http"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// TestSessionExportEndpoint: the export endpoint streams the session's
// event log as NDJSON with an attachment disposition; every line is a
// valid event carrying sequence and type.
func TestSessionExportEndpoint(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "hello world", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`)
	if status != http.StatusAccepted {
		t.Fatalf("submit status = %d, want 202", status)
	}
	waitIdle(t, ts, id)

	req, _ := http.NewRequest("GET", ts.URL+"/v1/sessions/"+id+"/export", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET export: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("export status = %d, want 200", resp.StatusCode)
	}
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/x-ndjson") {
		t.Fatalf("content-type = %q, want application/x-ndjson", ct)
	}
	if cd := resp.Header.Get("Content-Disposition"); !strings.Contains(cd, ".jsonl") {
		t.Fatalf("content-disposition = %q, want a .jsonl attachment", cd)
	}

	lines := 0
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 0, 1<<20), 1<<20)
	for sc.Scan() {
		line := bytes.TrimSpace(sc.Bytes())
		if len(line) == 0 {
			continue
		}
		var evt struct {
			Sequence int64  `json:"sequence"`
			Type     string `json:"type"`
		}
		if err := json.Unmarshal(line, &evt); err != nil {
			t.Fatalf("line %d is not a valid event: %v", lines+1, err)
		}
		if evt.Type == "" {
			t.Fatalf("line %d has no event type", lines+1)
		}
		lines++
	}
	if lines == 0 {
		t.Fatal("export carried no events after a completed turn")
	}
}

// TestSessionExportEndpointUnknown: an unknown session id surfaces the
// store's not-found error rather than an empty log.
func TestSessionExportEndpointUnknown(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	req, _ := http.NewRequest("GET", ts.URL+"/v1/sessions/"+domain.NewSessionID().String()+"/export", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET export: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("unknown session status = %d, want 404", resp.StatusCode)
	}
}
