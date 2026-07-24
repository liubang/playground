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
// Created: 2026/07/23

package render

import (
	"encoding/json"
	"fmt"
	"io"

	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// JSONL renders RuntimeEvents as one JSON object per line on stdout.
// This is the machine-readable output format suitable for piping to
// other tools, CI systems, or programmatic consumption. Only the
// protocol envelope and durable payloads are written; ephemeral
// events that do not represent durable state are optionally included.
//
// The output is newline-delimited JSON (NDJSON/JSONL): each line is
// a complete, valid JSON object. stdout receives only protocol data;
// logs and diagnostics go to stderr.
type JSONL struct {
	out              io.Writer
	includeEphemeral bool
	encoder          *json.Encoder
}

// JSONLOption configures a JSONL renderer.
type JSONLOption func(*JSONL)

// WithEphemeral includes ephemeral events (text deltas, tool progress)
// in the JSONL output. By default only durable events are emitted.
func WithEphemeral(include bool) JSONLOption {
	return func(j *JSONL) {
		j.includeEphemeral = include
	}
}

// NewJSONL creates a new JSONL renderer.
func NewJSONL(out io.Writer, opts ...JSONLOption) *JSONL {
	j := &JSONL{
		out:     out,
		encoder: json.NewEncoder(out),
	}
	for _, opt := range opts {
		opt(j)
	}
	return j
}

// ObserveEphemeral handles ephemeral events. When WithEphemeral(true)
// is set, these are written as JSONL lines; otherwise they are dropped
// silently (the consuming program can still reconstruct state from
// durable events alone).
func (j *JSONL) ObserveEphemeral(evt runtimeevent.RuntimeEvent) {
	if !j.includeEphemeral {
		return
	}
	j.emit(evt)
}

// ObserveDurable handles durable events. All durable events are written
// as JSONL lines regardless of the WithEphemeral setting.
func (j *JSONL) ObserveDurable(evt runtimeevent.RuntimeEvent) error {
	j.emit(evt)
	return nil
}

func (j *JSONL) emit(evt runtimeevent.RuntimeEvent) {
	// Encode the RuntimeEvent as a single JSON line.
	// Errors are unreachable for well-formed events (they're already
	// validated by the Broker), but log to stderr if they occur.
	if err := j.encoder.Encode(evt); err != nil {
		fmt.Fprintf(j.out, "{\"error\":\"encode failed: %s\"}\n", err.Error())
	}
}

// Flush is a no-op for the JSONL encoder (json.Encoder does not buffer),
// but is provided for interfaces that may require explicit flushing.
func (j *JSONL) Flush() error {
	// json.Encoder writes directly to the underlying io.Writer.
	// If the underlying writer implements Flush(), delegate.
	type flusher interface {
		Flush() error
	}
	if f, ok := j.out.(flusher); ok {
		return f.Flush()
	}
	return nil
}

// Close is a no-op for JSONL, but provided for interface compatibility.
// The caller is responsible for closing the underlying writer.
func (j *JSONL) Close() error {
	return j.Flush()
}
