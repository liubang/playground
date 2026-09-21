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

final class SSEParserTests: XCTestCase {
    private func feed(_ parser: inout SSEParser, _ string: String) -> [SSEFrame] {
        parser.feed(Array(string.utf8))
    }

    func testSingleEventFrame() {
        var parser = SSEParser()
        let frames = feed(&parser, "id: 1235\nevent: turn.started\ndata: {\"kind\":\"turn.started\"}\n\n")
        XCTAssertEqual(frames, [SSEFrame(content: .event(
            id: 1235, event: "turn.started", data: "{\"kind\":\"turn.started\"}",
        ))])
    }

    func testCommentFrames() {
        var parser = SSEParser()
        let frames = feed(&parser, ": connected, instance=7f3a9c\n\n: hb 1690000000\n\n")
        XCTAssertEqual(frames, [
            SSEFrame(content: .comment("connected, instance=7f3a9c")),
            SSEFrame(content: .comment("hb 1690000000")),
        ])
        XCTAssertEqual(frames[0].connectedInstance, "7f3a9c")
    }

    func testMultilineDataJoinsWithNewline() {
        var parser = SSEParser()
        let frames = feed(&parser, "event: x\ndata: line1\ndata: line2\n\n")
        XCTAssertEqual(frames, [SSEFrame(content: .event(id: nil, event: "x", data: "line1\nline2"))])
    }

    func testChunkedDeliveryAcrossFrameBoundary() {
        var parser = SSEParser()
        let chunks = [
            "id: 42\nev",
            "ent: model.text_delta\nda",
            "ta: {\"delta\":\"he",
            "llo\"}\n\nid: 43\nevent: x\ndata: y\n\n",
        ]
        var frames: [SSEFrame] = []
        for chunk in chunks {
            frames.append(contentsOf: feed(&parser, chunk))
        }
        XCTAssertEqual(frames, [
            SSEFrame(content: .event(id: 42, event: "model.text_delta", data: "{\"delta\":\"hello\"}")),
            SSEFrame(content: .event(id: 43, event: "x", data: "y")),
        ])
    }

    func testCRLFLineEndings() {
        var parser = SSEParser()
        let frames = feed(&parser, "id: 7\r\nevent: x\r\ndata: y\r\n\r\n")
        XCTAssertEqual(frames, [SSEFrame(content: .event(id: 7, event: "x", data: "y"))])
    }

    func testUnknownFieldsIgnoredForForwardCompatibility() {
        var parser = SSEParser()
        let frames = feed(&parser, "retry: 3000\nid: 9\nevent: x\ndata: y\n\n")
        XCTAssertEqual(frames, [SSEFrame(content: .event(id: 9, event: "x", data: "y"))])
    }

    func testBlankLineWithoutPendingEventYieldsNothing() {
        var parser = SSEParser()
        XCTAssertTrue(feed(&parser, "\n\n\n").isEmpty)
    }

    func testFinishFlushesUnterminatedTrailingEvent() {
        var parser = SSEParser()
        XCTAssertTrue(feed(&parser, "id: 1\nevent: x\ndata: tail").isEmpty)
        let frames = parser.finish()
        XCTAssertEqual(frames, [SSEFrame(content: .event(id: 1, event: "x", data: "tail"))])
    }

    func testServerControlEventsPassThrough() {
        var parser = SSEParser()
        let frames = feed(&parser, "event: server.resync\ndata: {\"reason\":\"cursor_invalid\"}\n\n")
        guard case let .event(_, event?, data) = frames.first?.content else {
            return XCTFail("expected event frame, got \(String(describing: frames.first))")
        }
        XCTAssertEqual(event, "server.resync")
        XCTAssertTrue(data.contains("cursor_invalid"))
    }
}
