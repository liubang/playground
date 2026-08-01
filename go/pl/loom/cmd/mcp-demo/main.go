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
// Created: 2026/08/01

// mcp-demo is a tiny standalone MCP server for exercising loom's MCP
// integration end to end (config -> bootstrap -> /mcp listing -> tool
// calls) without installing an external server. It speaks the stdio
// transport: newline-delimited JSON-RPC 2.0 on stdin/stdout, logs on
// stderr only (stdout is protocol-reserved).
//
// Wire it up in ~/.loom/config.yaml:
//
//	mcp_servers:
//	  demo:
//	    command: /absolute/path/to/mcp-demo
//
// It exposes three tools so listings and filters have something to show:
// echo, get_time and roll_dice.
package main

import (
	"bufio"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"math/big"
	"os"
	"time"
)

// protocolVersion must match a revision the loom client accepts; the
// methods used here (initialize, tools/list, tools/call) are stable
// across revisions.
const protocolVersion = "2025-06-18"

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      *int64          `json:"id,omitempty"`
	Method  string          `json:"method,omitempty"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type rpcResponse struct {
	JSONRPC string    `json:"jsonrpc"`
	ID      *int64    `json:"id"`
	Result  any       `json:"result,omitempty"`
	Error   *rpcError `json:"error,omitempty"`
}

type rpcError struct {
	Code    int64  `json:"code"`
	Message string `json:"message"`
}

type toolDef struct {
	Name        string         `json:"name"`
	Description string         `json:"description"`
	InputSchema map[string]any `json:"inputSchema"`
	// Annotations mark every demo tool read-only so loom's permission
	// engine treats calls as side-effect-free.
	Annotations map[string]any `json:"annotations,omitempty"`
}

var tools = []toolDef{
	{
		Name:        "echo",
		Description: "Echo the input text back, prefixed with the server name",
		InputSchema: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"text": map[string]any{
					"type":        "string",
					"description": "text to echo back",
				},
			},
			"required": []string{"text"},
		},
		Annotations: map[string]any{"readOnlyHint": true},
	},
	{
		Name:        "get_time",
		Description: "Return the server's current local time",
		InputSchema: map[string]any{
			"type":       "object",
			"properties": map[string]any{},
		},
		Annotations: map[string]any{"readOnlyHint": true},
	},
	{
		Name:        "roll_dice",
		Description: "Roll a dice with the given number of sides (default 6)",
		InputSchema: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"sides": map[string]any{
					"type":        "integer",
					"description": "number of sides, 2..100 (default 6)",
				},
			},
		},
		Annotations: map[string]any{"readOnlyHint": true},
	},
}

func main() {
	log("mcp-demo starting (pid %d)", os.Getpid())
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		var req rpcRequest
		if err := json.Unmarshal(line, &req); err != nil {
			log("dropping malformed message: %v", err)
			continue
		}
		handle(req)
	}
	if err := scanner.Err(); err != nil {
		log("stdin read error: %v", err)
	}
	log("stdin closed, exiting")
}

func handle(req rpcRequest) {
	switch req.Method {
	case "initialize":
		writeResult(req.ID, map[string]any{
			"protocolVersion": protocolVersion,
			"serverInfo":      map[string]any{"name": "loom-mcp-demo", "version": "0.1.0"},
			"capabilities":    map[string]any{"tools": map[string]any{}},
		})
		log("initialized (client protocol: %s)", paramString(req.Params, "protocolVersion"))
	case "notifications/initialized":
		// Notification: no response.
	case "ping":
		writeResult(req.ID, map[string]any{})
	case "tools/list":
		writeResult(req.ID, map[string]any{"tools": tools})
		log("tools/list -> %d tools", len(tools))
	case "tools/call":
		handleCall(req)
	default:
		if req.ID != nil {
			writeError(req.ID, -32601, fmt.Sprintf("method not found: %s", req.Method))
		}
	}
}

func handleCall(req rpcRequest) {
	var params struct {
		Name      string         `json:"name"`
		Arguments map[string]any `json:"arguments"`
	}
	if err := json.Unmarshal(req.Params, &params); err != nil {
		writeError(req.ID, -32602, "invalid tools/call params")
		return
	}
	log("tools/call %s", params.Name)
	switch params.Name {
	case "echo":
		text, _ := params.Arguments["text"].(string)
		writeText(req.ID, "demo says: "+text)
	case "get_time":
		writeText(req.ID, time.Now().Format(time.RFC3339))
	case "roll_dice":
		sides := 6
		if v, ok := params.Arguments["sides"].(float64); ok {
			sides = int(v)
		}
		if sides < 2 || sides > 100 {
			writeErrorText(req.ID, fmt.Sprintf("sides must be in [2, 100], got %d", sides))
			return
		}
		n, err := rand.Int(rand.Reader, big.NewInt(int64(sides)))
		if err != nil {
			writeErrorText(req.ID, "rng failure: "+err.Error())
			return
		}
		writeText(req.ID, fmt.Sprintf("rolled a d%d: %d", sides, n.Int64()+1))
	default:
		writeError(req.ID, -32602, fmt.Sprintf("unknown tool: %s", params.Name))
	}
}

func writeText(id *int64, text string) {
	writeResult(id, map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
	})
}

// writeErrorText reports a tool-level failure (isError content), which is
// how MCP servers surface domain errors the model can recover from, as
// opposed to protocol errors.
func writeErrorText(id *int64, text string) {
	writeResult(id, map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
		"isError": true,
	})
}

func writeResult(id *int64, result any) {
	write(rpcResponse{JSONRPC: "2.0", ID: id, Result: result})
}

func writeError(id *int64, code int64, message string) {
	write(rpcResponse{JSONRPC: "2.0", ID: id, Error: &rpcError{Code: code, Message: message}})
}

func write(resp rpcResponse) {
	data, err := json.Marshal(resp)
	if err != nil {
		log("marshal response failed: %v", err)
		return
	}
	fmt.Fprintf(os.Stdout, "%s\n", data)
}

func paramString(params json.RawMessage, key string) string {
	var m map[string]any
	if err := json.Unmarshal(params, &m); err != nil {
		return ""
	}
	s, _ := m[key].(string)
	return s
}

// log writes to stderr: stdout is reserved for protocol frames, so any
// diagnostics (picked up by loom's debug log) must go here.
func log(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "[mcp-demo] "+format+"\n", args...)
}
