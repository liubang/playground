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

import Foundation

// MARK: - SSE frame model (SERVE_DESIGN §5.4)

/// One parsed SSE frame. Loom's stream uses three shapes:
///   - comments: `: connected, instance=<id>` and `: hb <unix>` keepalives
///   - runtime events: `id:`/`event:`/`data:` triplets wrapping RuntimeEvent
///   - server control events: `server.resync` / `server.draining`
///     (not RuntimeEvents; never enter the replay log)
struct SSEFrame: Equatable, Sendable {
    enum Content: Equatable, Sendable {
        /// A comment line (`:` prefix). Value excludes the leading `:` and
        /// one optional space, e.g. "connected, instance=7f3a9c" / "hb 169…".
        case comment(String)
        /// A complete event frame. `id` is the global sequence cursor.
        case event(id: UInt64?, event: String?, data: String)
    }

    let content: Content
}

extension SSEFrame {
    /// The `: connected, instance=<id>` handshake, if this frame is one.
    var connectedInstance: String? {
        guard case let .comment(text) = content,
              text.hasPrefix("connected, instance=") else { return nil }
        return String(text.dropFirst("connected, instance=".count))
            .trimmingCharacters(in: .whitespaces)
    }
}

// MARK: - Incremental parser

/// Byte-level SSE parser (RFC 8890 / WHATWG SSE): feed arbitrary byte
/// chunks, get complete frames out. Pure value type, fully unit-testable.
struct SSEParser: Sendable {
    private var buffer = Data()
    private var pendingId: String?
    private var pendingEvent: String?
    private var pendingData: [String] = []

    init() {}

    /// Feeds a chunk; returns all frames completed by it.
    mutating func feed(_ bytes: some Sequence<UInt8>) -> [SSEFrame] {
        buffer.append(contentsOf: bytes)
        var frames: [SSEFrame] = []
        while let newline = buffer.firstIndex(of: 0x0A) {
            var end = newline
            if end > buffer.startIndex, buffer[buffer.index(before: end)] == 0x0D {
                end = buffer.index(before: end)
            }
            // Process before mutating the buffer: the subsequence shares
            // storage with it.
            if let frame = processLine(buffer[buffer.startIndex ..< end]) {
                frames.append(frame)
            }
            // Consume the line *and* its line ending — dropping only the
            // content range would leave the \n behind and spin forever.
            buffer.removeSubrange(buffer.startIndex ... newline)
        }
        return frames
    }

    /// End-of-stream: flush complete lines still buffered, then the
    /// unterminated tail line, then dispatch the pending event (WHATWG
    /// SSE: EOF dispatches).
    mutating func finish() -> [SSEFrame] {
        var frames = feed([])
        if !buffer.isEmpty {
            let tail = buffer
            buffer.removeAll()
            if let frame = processLine(tail[...]) {
                frames.append(frame)
            }
        }
        if let frame = dispatchPending() {
            frames.append(frame)
        }
        return frames
    }

    /// Dispatches the accumulated event fields, if any (blank line / EOF).
    private mutating func dispatchPending() -> SSEFrame? {
        guard pendingId != nil || pendingEvent != nil || !pendingData.isEmpty else { return nil }
        let frame = SSEFrame(content: .event(
            id: pendingId.flatMap(UInt64.init),
            event: pendingEvent,
            data: pendingData.joined(separator: "\n"),
        ))
        pendingId = nil
        pendingEvent = nil
        pendingData = []
        return frame
    }

    private mutating func processLine(_ data: Data.SubSequence) -> SSEFrame? {
        // Blank line: dispatch the accumulated event, if any.
        if data.isEmpty {
            return dispatchPending()
        }

        let line = String(decoding: data, as: UTF8.self)

        // Comment line.
        if line.hasPrefix(":") {
            var comment = line
            comment.removeFirst()
            if comment.hasPrefix(" ") {
                comment.removeFirst()
            }
            return SSEFrame(content: .comment(comment))
        }

        // field: value — one optional leading space is stripped.
        if let colon = line.firstIndex(of: ":") {
            let field = String(line[line.startIndex ..< colon])
            var value = String(line[line.index(after: colon)...])
            if value.hasPrefix(" ") {
                value.removeFirst()
            }
            switch field {
            case "id": pendingId = value
            case "event": pendingEvent = value
            case "data": pendingData.append(value)
            default: break // forward-compatible: ignore unknown fields
            }
        }
        return nil
    }
}

// MARK: - Streaming client

/// Opens `GET /v1/sessions/{id}/events` and yields parsed frames until the
/// server closes the stream or the consuming task is cancelled. Reconnect
/// policy (backoff, resync, watchdog) belongs to the store layer — this
/// type only guarantees correctly-parsed frames and prompt byte flow.
struct SSEClient: Sendable {
    let baseURL: URL
    let token: String

    init(baseURL: URL, token: String) {
        var base = baseURL.absoluteString
        while base.hasSuffix("/") {
            base.removeLast()
        }
        self.baseURL = URL(string: base)!
        self.token = token
    }

    enum StreamError: Error, Sendable {
        /// 404 — session not alive in this server process; caller must
        /// resume the session then re-attach (sse.ts:130-137).
        case sessionNotAlive
        /// 429 — per-session SSE cap; caller should back off hard (~30s).
        case rateLimited
        case http(Int)
    }

    func frames(sessionId: String, after: UInt64) -> AsyncThrowingStream<SSEFrame, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                var components = URLComponents(
                    url: baseURL.appendingPathComponent("/v1/sessions/\(sessionId)/events"),
                    resolvingAgainstBaseURL: false,
                )!
                components.queryItems = [URLQueryItem(name: "after", value: String(after))]

                var request = URLRequest(url: components.url!)
                request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                request.setValue("text/event-stream", forHTTPHeaderField: "Accept")
                // No timeout: SSE is a long-lived response (server §5.1).
                request.timeoutInterval = .infinity

                do {
                    let (bytes, response) = try await URLSession.shared.bytes(for: request)
                    let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                    guard status == 200 else {
                        switch status {
                        case 404: continuation.finish(throwing: StreamError.sessionNotAlive)
                        case 429: continuation.finish(throwing: StreamError.rateLimited)
                        default: continuation.finish(throwing: StreamError.http(status))
                        }
                        return
                    }

                    var parser = SSEParser()
                    // Feed raw bytes, flushing at newline boundaries.
                    // Do NOT "optimize" this into AsyncBytes.lines:
                    // AsyncLineSequence drops EMPTY lines, and SSE
                    // events are dispatched by the blank line that
                    // terminates them — through .lines the parser
                    // accumulates id/event/data forever and no runtime
                    // event is ever emitted (only comments, which are
                    // per-line, got through: the badge went "live"
                    // while every event silently died).
                    var chunk: [UInt8] = []
                    chunk.reserveCapacity(4096)
                    for try await byte in bytes {
                        try Task.checkCancellation()
                        chunk.append(byte)
                        if byte == 0x0A || chunk.count >= 4096 {
                            for frame in parser.feed(chunk) {
                                continuation.yield(frame)
                            }
                            chunk.removeAll(keepingCapacity: true)
                        }
                    }
                    for frame in parser.feed(chunk) + parser.finish() {
                        continuation.yield(frame)
                    }
                    continuation.finish()
                } catch is CancellationError {
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
