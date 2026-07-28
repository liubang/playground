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

// Package sse implements the Server-Sent Events wire format shared by the
// streaming model providers. It handles the rough edges real gateways
// exhibit: comment/heartbeat lines, multi-line data fields, and a final
// event flushed by EOF instead of a blank line.
package sse

import (
	"bufio"
	"errors"
	"io"
	"strings"
)

// Event is one parsed SSE frame. Name is the "event" field (may be empty);
// Data is the concatenation of all "data" lines.
type Event struct {
	Name string
	Data string
}

// Parser reads events from an SSE stream.
type Parser struct {
	reader *bufio.Reader
}

// NewParser returns a Parser over r.
func NewParser(r io.Reader) *Parser {
	return &Parser{reader: bufio.NewReader(r)}
}

// Next returns the next event, or io.EOF once the stream is cleanly
// exhausted. Any other error indicates a malformed frame or a read failure.
func (p *Parser) Next() (Event, error) {
	var (
		dataLines []string
		eventName string
	)

	for {
		line, err := p.reader.ReadString('\n')
		eof := errors.Is(err, io.EOF)
		if err != nil && !eof {
			return Event{}, err
		}

		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			if len(dataLines) == 0 {
				// Dispatch with an empty buffer: the spec discards the event
				// and resets the name so it cannot leak into the next one.
				eventName = ""
				if eof {
					return Event{}, io.EOF
				}
				continue
			}
			return Event{Name: eventName, Data: strings.Join(dataLines, "\n")}, nil
		}

		if strings.HasPrefix(line, ":") {
			if eof {
				if len(dataLines) == 0 {
					return Event{}, io.EOF
				}
				return Event{Name: eventName, Data: strings.Join(dataLines, "\n")}, nil
			}
			continue
		}

		field, value, found := strings.Cut(line, ":")
		if !found {
			// Spec: a line without a colon is a field name with an empty value.
			field, value = line, ""
		}
		if strings.HasPrefix(value, " ") {
			value = value[1:]
		}

		switch field {
		case "data":
			dataLines = append(dataLines, value)
		case "event":
			eventName = value
		default:
			// Spec: unknown fields (id, retry, and anything a gateway adds)
			// are ignored, not fatal.
		}

		if eof {
			if len(dataLines) == 0 {
				return Event{}, io.EOF
			}
			return Event{Name: eventName, Data: strings.Join(dataLines, "\n")}, nil
		}
	}
}
