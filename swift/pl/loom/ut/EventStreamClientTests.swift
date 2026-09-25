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

@testable import Loom
import XCTest

/// EventStreamClient 的纯逻辑面：WS 握手请求构造（scheme 改写、游标、
/// Authorization 头）与断流后的传输决策（WS 优先、未开张回退 SSE）。
final class EventStreamClientTests: XCTestCase {
    func testWebSocketRequestBuildsWSURLWithBearerAndCursor() throws {
        let request = try EventStreamClient.webSocketRequest(
            baseURL: XCTUnwrap(URL(string: "http://127.0.0.1:7680")),
            token: "sekrit",
            sessionId: "sess_abc",
            after: 42,
        )
        XCTAssertEqual(
            request.url?.absoluteString,
            "ws://127.0.0.1:7680/v1/sessions/sess_abc/events?after=42",
        )
        XCTAssertEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer sekrit")
        XCTAssertEqual(request.timeoutInterval, .infinity)
    }

    func testWebSocketRequestRewritesHTTPSToWSS() throws {
        let request = try EventStreamClient.webSocketRequest(
            baseURL: XCTUnwrap(URL(string: "https://loom.example:8443/")),
            token: "",
            sessionId: "sess_x",
            after: 0,
        )
        XCTAssertEqual(request.url?.scheme, "wss")
        XCTAssertEqual(request.url?.host, "loom.example")
        XCTAssertEqual(request.url?.port, 8443)
        XCTAssertNil(request.value(forHTTPHeaderField: "Authorization"))
    }

    func testStreamOutcomeClassification() throws {
        let url = try XCTUnwrap(URL(string: "http://127.0.0.1:7680"))
        // 握手被拒（真实 HTTP 状态，如 401/404/429）→ 回退 SSE 拿精确语义。
        let rejected = HTTPURLResponse(url: url, statusCode: 429, httpVersion: nil, headerFields: nil)
        XCTAssertEqual(
            EventStreamClient.streamOutcome(response: rejected, receivedAnyFrame: false, cancelled: false),
            .failedBeforeOpen,
        )
        // 101 之后一帧未到就断了（老式服务器/吃 upgrade 的代理）→ 同样回退。
        let upgraded = HTTPURLResponse(url: url, statusCode: 101, httpVersion: nil, headerFields: nil)
        XCTAssertEqual(
            EventStreamClient.streamOutcome(response: upgraded, receivedAnyFrame: false, cancelled: false),
            .failedBeforeOpen,
        )
        // 流过帧之后中途断流 → 交给 store 的重连策略，下次仍先试 WS。
        XCTAssertEqual(
            EventStreamClient.streamOutcome(response: upgraded, receivedAnyFrame: true, cancelled: false),
            .dropped,
        )
        // 消费者取消无论处于何阶段都不再回退/重试。
        XCTAssertEqual(
            EventStreamClient.streamOutcome(response: nil, receivedAnyFrame: true, cancelled: true),
            .closed,
        )
        XCTAssertEqual(
            EventStreamClient.streamOutcome(response: nil, receivedAnyFrame: false, cancelled: true),
            .closed,
        )
    }
}
