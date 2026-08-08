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
// Created: 2026/08/03

package mcp

import "context"

// transport moves JSON-RPC messages between the Client and one MCP
// server, hiding how bytes travel (subprocess pipes or HTTP POSTs).
// Message framing is the transport's job: the stdio transport delimits
// messages with newlines, the streamable HTTP transport sends one
// message per POST body.
type transport interface {
	// roundTrip sends one request and returns the response whose id
	// matches. Interleaved notifications and server->client requests
	// are consumed and logged, never delivered: loom advertises no
	// client capabilities. The returned error is nil unless the
	// transport itself failed or ctx was done — a JSON-RPC error
	// travels inside the returned rpcMessage.
	roundTrip(ctx context.Context, id int64, request []byte) (rpcMessage, error)
	// notify sends a notification; no response is expected or awaited.
	notify(ctx context.Context, notification []byte) error
	// adoptProtocolVersion records the protocol revision the server
	// negotiated at initialize; only the HTTP transport replays it (as
	// the MCP-Protocol-Version header), stdio ignores it.
	adoptProtocolVersion(version string)
	// close shuts the transport down; in-flight round trips fail fast.
	close() error
}
