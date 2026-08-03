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
// calls) without installing an external server. It speaks both
// transports:
//
//   - stdio (default): newline-delimited JSON-RPC 2.0 on stdin/stdout,
//     logs on stderr only (stdout is protocol-reserved);
//   - streamable HTTP (-http ADDR): one POST endpoint per spec
//     2025-06-18, with optional bearer auth (-token) and an SSE response
//     mode (-sse) so both response shapes are exercisable.
//
// Wire it up in ~/.loom/config.yaml:
//
//	mcp_servers:
//	  demo:
//	    command: /absolute/path/to/mcp-demo            # stdio
//	  demo-http:
//	    url: http://127.0.0.1:8931/mcp                 # streamable HTTP
//	    headers:
//	      Authorization: Bearer ${MCP_DEMO_TOKEN}
//
// It exposes three tools so listings and filters have something to show:
// echo, get_time and roll_dice.
package main

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"os"
	"sync"
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
	httpAddr := flag.String("http", "", "serve the streamable HTTP transport on this address (e.g. 127.0.0.1:8931) instead of stdio")
	token := flag.String("token", "", "require this bearer token on HTTP requests (streamable HTTP mode only)")
	sseMode := flag.Bool("sse", false, "answer requests with a text/event-stream instead of a bare JSON body (streamable HTTP mode only)")
	flag.Parse()

	if *httpAddr != "" {
		serveHTTP(*httpAddr, *token, *sseMode)
		return
	}
	serveStdio()
}

// -----------------------------------------------------------------------
// Protocol core (shared by both transports)
// -----------------------------------------------------------------------

// dispatch answers one request message; it returns nil for notifications.
func dispatch(req rpcRequest) *rpcResponse {
	switch req.Method {
	case "initialize":
		log("initialized (client protocol: %s)", paramString(req.Params, "protocolVersion"))
		return resultResponse(req.ID, map[string]any{
			"protocolVersion": protocolVersion,
			"serverInfo":      map[string]any{"name": "loom-mcp-demo", "version": "0.1.0"},
			"capabilities":    map[string]any{"tools": map[string]any{}},
		})
	case "notifications/initialized":
		return nil // Notification: no response.
	case "ping":
		return resultResponse(req.ID, map[string]any{})
	case "tools/list":
		log("tools/list -> %d tools", len(tools))
		return resultResponse(req.ID, map[string]any{"tools": tools})
	case "tools/call":
		return dispatchCall(req)
	default:
		if req.ID == nil {
			return nil
		}
		return errorResponse(req.ID, -32601, fmt.Sprintf("method not found: %s", req.Method))
	}
}

func dispatchCall(req rpcRequest) *rpcResponse {
	var params struct {
		Name      string         `json:"name"`
		Arguments map[string]any `json:"arguments"`
	}
	if err := json.Unmarshal(req.Params, &params); err != nil {
		return errorResponse(req.ID, -32602, "invalid tools/call params")
	}
	log("tools/call %s", params.Name)
	switch params.Name {
	case "echo":
		text, _ := params.Arguments["text"].(string)
		return textResponse(req.ID, "demo says: "+text)
	case "get_time":
		return textResponse(req.ID, time.Now().Format(time.RFC3339))
	case "roll_dice":
		sides := 6
		if v, ok := params.Arguments["sides"].(float64); ok {
			sides = int(v)
		}
		if sides < 2 || sides > 100 {
			return errorTextResponse(req.ID, fmt.Sprintf("sides must be in [2, 100], got %d", sides))
		}
		n, err := rand.Int(rand.Reader, big.NewInt(int64(sides)))
		if err != nil {
			return errorTextResponse(req.ID, "rng failure: "+err.Error())
		}
		return textResponse(req.ID, fmt.Sprintf("rolled a d%d: %d", sides, n.Int64()+1))
	default:
		return errorResponse(req.ID, -32602, fmt.Sprintf("unknown tool: %s", params.Name))
	}
}

func textResponse(id *int64, text string) *rpcResponse {
	return resultResponse(id, map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
	})
}

// errorTextResponse reports a tool-level failure (isError content), which
// is how MCP servers surface domain errors the model can recover from,
// as opposed to protocol errors.
func errorTextResponse(id *int64, text string) *rpcResponse {
	return resultResponse(id, map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
		"isError": true,
	})
}

func resultResponse(id *int64, result any) *rpcResponse {
	return &rpcResponse{JSONRPC: "2.0", ID: id, Result: result}
}

func errorResponse(id *int64, code int64, message string) *rpcResponse {
	return &rpcResponse{JSONRPC: "2.0", ID: id, Error: &rpcError{Code: code, Message: message}}
}

func paramString(params json.RawMessage, key string) string {
	var m map[string]any
	if err := json.Unmarshal(params, &m); err != nil {
		return ""
	}
	s, _ := m[key].(string)
	return s
}

// log writes to stderr: on stdio stdout is reserved for protocol frames,
// and on HTTP stderr is where loom's server log collection looks.
func log(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "[mcp-demo] "+format+"\n", args...)
}

// -----------------------------------------------------------------------
// stdio transport
// -----------------------------------------------------------------------

func serveStdio() {
	log("mcp-demo starting on stdio (pid %d)", os.Getpid())
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
		resp := dispatch(req)
		if resp == nil {
			continue
		}
		data, err := json.Marshal(resp)
		if err != nil {
			log("marshal response failed: %v", err)
			continue
		}
		fmt.Fprintf(os.Stdout, "%s\n", data)
	}
	if err := scanner.Err(); err != nil {
		log("stdin read error: %v", err)
	}
	log("stdin closed, exiting")
}

// -----------------------------------------------------------------------
// Streamable HTTP transport (spec 2025-06-18)
// -----------------------------------------------------------------------

// httpServer implements the single-endpoint streamable HTTP transport:
// every message arrives as a POST; requests answer with JSON or an SSE
// stream, notifications with 202, and DELETE terminates the session.
type httpServer struct {
	token string // required bearer token; empty disables auth
	sse   bool   // answer requests with text/event-stream

	mu      sync.Mutex
	session string // issued at initialize; empty until then
}

func serveHTTP(addr, token string, sseMode bool) {
	srv := &httpServer{token: token, sse: sseMode}
	mux := http.NewServeMux()
	mux.Handle("/mcp", srv)
	log("mcp-demo serving streamable HTTP on %s/mcp (auth=%v, sse=%v)", addr, token != "", sseMode)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log("http server failed: %v", err)
		os.Exit(1)
	}
}

func (s *httpServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	switch r.Method {
	case http.MethodPost:
		s.handlePost(w, r)
	case http.MethodDelete:
		s.mu.Lock()
		s.session = ""
		s.mu.Unlock()
		log("session terminated")
		w.WriteHeader(http.StatusOK)
	default:
		w.Header().Set("Allow", "POST, DELETE")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *httpServer) authorized(r *http.Request) bool {
	if s.token == "" {
		return true
	}
	return r.Header.Get("Authorization") == "Bearer "+s.token
}

// validSession enforces the session contract once a session exists: a
// request carrying a missing or wrong Mcp-Session-Id gets a 404, which
// tells the client to re-initialize.
func (s *httpServer) validSession(r *http.Request) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.session == "" {
		return true
	}
	return r.Header.Get("Mcp-Session-Id") == s.session
}

func (s *httpServer) handlePost(w http.ResponseWriter, r *http.Request) {
	if !s.validSession(r) {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 16<<20))
	if err != nil {
		http.Error(w, "read body failed", http.StatusBadRequest)
		return
	}
	var req rpcRequest
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, "malformed JSON-RPC message", http.StatusBadRequest)
		return
	}

	resp := dispatch(req)
	if resp == nil { // notification
		w.WriteHeader(http.StatusAccepted)
		return
	}

	if req.Method == "initialize" {
		s.mu.Lock()
		s.session = newSessionID()
		sid := s.session
		s.mu.Unlock()
		w.Header().Set("Mcp-Session-Id", sid)
		log("session %s issued", sid)
	}

	data, err := json.Marshal(resp)
	if err != nil {
		http.Error(w, "marshal response failed", http.StatusInternalServerError)
		return
	}
	if s.sse {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprintf(w, "event: message\ndata: %s\n\n", data)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(data)
}

func newSessionID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return fmt.Sprintf("fallback-%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(b[:])
}
