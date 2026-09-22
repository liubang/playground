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

/// Regression lock for the PUT /v1/config body encoding: the server
/// parses it with gopkg.in/yaml.v3, which rejects Foundation's \/
/// escape ("found unknown escape character") — the body must not
/// contain that byte sequence, and must still decode losslessly.
final class ConfigBodyEncodingTests: XCTestCase {
    func testNoEscapedSlashes() throws {
        let data = try APIClient.configBodyData(
            revision: "rev",
            config: .object([
                "base_url": .string("https://api.deepseek.com/v1"),
                "nested": .object(["path": .string("a/b/c")]),
            ]),
        )
        let text = String(decoding: data, as: UTF8.self)
        XCTAssertFalse(text.contains("\\/"), "body must not carry the \\/ escape: \(text)")
        XCTAssertTrue(text.contains("https://api.deepseek.com/v1"))
    }

    /// The literal backslash+slash sequence survives the strip:
    /// `\` encodes as \\, so `\/` only ever means an escaped slash.
    func testLiteralBackslashSlashRoundTrip() throws {
        let config = JSONValue.object(["v": .string("x\\/y")]) // x\ + /y
        let data = try APIClient.configBodyData(revision: "", config: config)
        let decoded = try JSONDecoder().decode(JSONValue.self, from: data)
        guard case let .object(body) = decoded,
              body["config"] == config
        else {
            return XCTFail("round trip changed the payload")
        }
    }
}
