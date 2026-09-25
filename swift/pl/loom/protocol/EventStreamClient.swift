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

// MARK: - Transport-composing event stream client

/// EventStreamClient — the session event stream over the best available
/// transport: WebSocket first, SSE fallback (the twin of the webui's
/// protocol/sse.ts and the Go server's handlers_events.go WS branch).
///
/// Why WebSocket: every URLSession SSE stream pins one HTTP/1.1
/// connection for its whole lifetime, and URLSession caps concurrent
/// connections per host (httpMaximumConnectionsPerHost, 6). A handful of
/// live session streams starve the pool, after which plain API calls —
/// prompt submit included — queue client-side until they hit their 30s
/// timeout and surface as "The request timed out." without ever reaching
/// the server (the 2026-09 "Action couldn't complete" incident).
/// URLSessionWebSocketTask connections do not count against that pool.
/// The server upgrades the events endpoint in place; each WS text
/// message carries exactly one SSE frame, so the proven SSEParser/store
/// pipeline is shared verbatim (SERVE_DESIGN §5.4).
struct EventStreamClient: Sendable {
    let baseURL: URL
    let token: String
    private let session: URLSession
    private let fallback: SSEClient
    /// Shared "WS works here" verdict: a pre-open failure disables the WS
    /// attempt for the lifetime of this client (old server, WS-blind
    /// proxy), so steady-state reconnects go straight to SSE instead of
    /// paying a doomed handshake per reconnect.
    private let wsUsability: WSUsability

    init(baseURL: URL, token: String, session: URLSession = .shared) {
        var base = baseURL.absoluteString
        while base.hasSuffix("/") {
            base.removeLast()
        }
        self.baseURL = URL(string: base)!
        self.token = token
        self.session = session
        fallback = SSEClient(baseURL: baseURL, token: token)
        wsUsability = WSUsability()
    }

    /// Why one WS connection ended — decides the next transport.
    enum WSStreamOutcome: Sendable, Equatable {
        /// Consumer cancellation, or the server closed after a clean run:
        /// the stream is over; the store's reconnect policy takes over.
        case closed
        /// The socket never delivered a single frame (handshake rejected,
        /// old server answering plain SSE, WS-blind proxy): fall back to
        /// SSE, which surfaces the precise HTTP status (404 resume / 429
        /// backoff) the WS API hides from us.
        case failedBeforeOpen
        /// The socket streamed and then died mid-flight: propagate the
        /// interruption; the reconnect retries WS (it worked here).
        case dropped
    }

    /// Classifies a failed/closed WS receive loop. A rejected handshake
    /// leaves the server's real HTTP response on the task (status != 101).
    static func streamOutcome(
        response: HTTPURLResponse?, receivedAnyFrame: Bool, cancelled: Bool,
    ) -> WSStreamOutcome {
        if cancelled {
            return .closed
        }
        if let response, response.statusCode != 101 {
            return .failedBeforeOpen
        }
        return receivedAnyFrame ? .dropped : .failedBeforeOpen
    }

    /// The WS handshake request: same URL/auth as the SSE GET, scheme
    /// swapped to ws(s). Unlike the browser WebSocket API, a
    /// URLSessionWebSocketTask request can carry headers, so the bearer
    /// token rides the Authorization header here — the query-token form is
    /// reserved for browsers (server auth.go).
    static func webSocketRequest(baseURL: URL, token: String, sessionId: String, after: UInt64) -> URLRequest {
        var components = URLComponents(
            url: baseURL.appendingPathComponent("/v1/sessions/\(sessionId)/events"),
            resolvingAgainstBaseURL: false,
        )!
        components.scheme = components.scheme == "https" ? "wss" : "ws"
        components.queryItems = [URLQueryItem(name: "after", value: String(after))]
        var request = URLRequest(url: components.url!)
        if !token.isEmpty {
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        // No timeout: the event stream is a long-lived connection.
        request.timeoutInterval = .infinity
        return request
    }

    /// Yields parsed frames until the stream ends or the consuming task is
    /// cancelled — the exact contract of SSEClient.frames, so the store
    /// layer (reconnect policy, resync, watchdog) is unchanged.
    func frames(sessionId: String, after: UInt64) -> AsyncThrowingStream<SSEFrame, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                if wsUsability.usable {
                    let outcome = await streamWS(sessionId: sessionId, after: after) { frame in
                        continuation.yield(frame)
                    }
                    switch outcome {
                    case .closed, .dropped:
                        continuation.finish()
                        return
                    case .failedBeforeOpen:
                        wsUsability.disable()
                    }
                }
                guard !Task.isCancelled else {
                    continuation.finish()
                    return
                }
                do {
                    for try await frame in fallback.frames(sessionId: sessionId, after: after) {
                        continuation.yield(frame)
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    /// One WS connection to completion: handshake, then feed every text
    /// message through the shared SSEParser (one SSE frame per message).
    private func streamWS(
        sessionId: String, after: UInt64,
        yield: @Sendable (SSEFrame) -> Void,
    ) async -> WSStreamOutcome {
        let request = Self.webSocketRequest(baseURL: baseURL, token: token, sessionId: sessionId, after: after)
        let task = session.webSocketTask(with: request)
        // Server frames carry full RuntimeEvent JSON — tool results can be
        // megabytes, and the default 1 MB message cap would kill the socket.
        task.maximumMessageSize = 64 * 1024 * 1024
        task.resume()
        return await withTaskCancellationHandler {
            var parser = SSEParser()
            var receivedAny = false
            do {
                while true {
                    let message = try await task.receive()
                    switch message {
                    case let .string(text):
                        for frame in parser.feed(text.utf8) {
                            yield(frame)
                        }
                    case let .data(data):
                        for frame in parser.feed(data) {
                            yield(frame)
                        }
                    @unknown default:
                        continue
                    }
                    receivedAny = true
                }
            } catch {
                // EOF parity with the SSE client: dispatch anything the
                // parser still holds before classifying the outcome.
                for frame in parser.finish() {
                    yield(frame)
                }
                return Self.streamOutcome(
                    response: task.response as? HTTPURLResponse,
                    receivedAnyFrame: receivedAny,
                    cancelled: Task.isCancelled,
                )
            }
        } onCancel: {
            task.cancel(with: .normalClosure, reason: nil)
        }
    }
}

/// Lock-protected shared verdict for "the WS transport works against this
/// server" — EventStreamClient is a Sendable value type, so the mutable
/// verdict lives behind a reference.
private final class WSUsability: @unchecked Sendable {
    private let lock = NSLock()
    private var value = true

    var usable: Bool {
        lock.lock()
        defer { lock.unlock() }
        return value
    }

    func disable() {
        lock.lock()
        value = false
        lock.unlock()
    }
}
