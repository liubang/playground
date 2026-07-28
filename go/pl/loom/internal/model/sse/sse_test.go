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
// Created: 2026/07/27

package sse

import (
	"errors"
	"io"
	"strings"
	"testing"
)

func collect(t *testing.T, input string) ([]Event, error) {
	t.Helper()
	p := NewParser(strings.NewReader(input))
	var events []Event
	for {
		evt, err := p.Next()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return events, nil
			}
			return events, err
		}
		events = append(events, evt)
	}
}

func TestParserBasicEvents(t *testing.T) {
	events, err := collect(t, "data: hello\n\ndata: world\n\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 2 || events[0].Data != "hello" || events[1].Data != "world" {
		t.Fatalf("events = %+v", events)
	}
	if events[0].Name != "" {
		t.Fatalf("unexpected event name %q", events[0].Name)
	}
}

// Regression (REVIEW M4): the parser used to hard-fail on lines without a
// colon and on unknown fields, both of which the WHATWG SSE spec says to
// tolerate — a gateway adding a new field (e.g. "meta:") would kill the
// whole stream.
func TestParserToleratesUnknownFieldsAndBareLines(t *testing.T) {
	events, err := collect(t, "meta: trace-1\nbareline\ndata: hello\n\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 1 || events[0].Data != "hello" {
		t.Fatalf("events = %+v", events)
	}
}

// Regression (REVIEW M4): a data-less named event is dispatched (and
// discarded) at the blank line; its name must not leak into the next event.
func TestParserResetsEventNameAfterEmptyDispatch(t *testing.T) {
	events, err := collect(t, "event: stale\n\ndata: x\n\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 1 || events[0].Data != "x" {
		t.Fatalf("events = %+v", events)
	}
	if events[0].Name != "" {
		t.Fatalf("event name leaked from the discarded event: %q", events[0].Name)
	}
}

func TestParserNamedEventAndMultilineData(t *testing.T) {
	events, err := collect(t, "event: message_start\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 1 {
		t.Fatalf("expected 1 event, got %d", len(events))
	}
	if events[0].Name != "message_start" {
		t.Fatalf("name = %q", events[0].Name)
	}
	if events[0].Data != "{\"a\":1}\n{\"b\":2}" {
		t.Fatalf("data = %q", events[0].Data)
	}
}

func TestParserSkipsCommentsAndIdRetry(t *testing.T) {
	events, err := collect(t, ": heartbeat\nid: 42\nretry: 1000\ndata: x\n\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 1 || events[0].Data != "x" {
		t.Fatalf("events = %+v", events)
	}
}

func TestParserFlushesFinalEventOnEOF(t *testing.T) {
	events, err := collect(t, "data: trailing")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 1 || events[0].Data != "trailing" {
		t.Fatalf("events = %+v", events)
	}
}

func TestParserEmptyStream(t *testing.T) {
	events, err := collect(t, "")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("events = %+v", events)
	}
}

// Per the WHATWG SSE spec, a line without a colon is a field name with an
// empty value, and unknown fields are ignored — neither is fatal.
func TestParserToleratesBareLineAndUnknownField(t *testing.T) {
	events, err := collect(t, "garbage-line-without-colon\nbogus: x\ndata: y\n\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 1 || events[0].Data != "y" {
		t.Fatalf("events = %+v", events)
	}
}

func TestParserCRLF(t *testing.T) {
	events, err := collect(t, "data: a\r\n\r\ndata: b\r\n\r\n")
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	if len(events) != 2 || events[0].Data != "a" || events[1].Data != "b" {
		t.Fatalf("events = %+v", events)
	}
}
