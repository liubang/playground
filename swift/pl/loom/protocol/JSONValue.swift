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

/// Schema-less JSON value for wire fields whose shape is open-ended
/// (tool `arguments`, event payloads before they are materialized).
/// Mirrors the encoding Go's `encoding/json` produces.
indirect enum JSONValue: Codable, Equatable, Sendable {
    case null
    case bool(Bool)
    case int(Int64)
    case double(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
        } else if let b = try? container.decode(Bool.self) {
            self = .bool(b)
        } else if let i = try? container.decode(Int64.self) {
            self = .int(i)
        } else if let d = try? container.decode(Double.self) {
            self = .double(d)
        } else if let s = try? container.decode(String.self) {
            self = .string(s)
        } else if let a = try? container.decode([JSONValue].self) {
            self = .array(a)
        } else {
            self = try .object(container.decode([String: JSONValue].self))
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .null: try container.encodeNil()
        case let .bool(b): try container.encode(b)
        case let .int(i): try container.encode(i)
        case let .double(d): try container.encode(d)
        case let .string(s): try container.encode(s)
        case let .array(a): try container.encode(a)
        case let .object(o): try container.encode(o)
        }
    }

    var stringValue: String? {
        if case let .string(s) = self {
            return s
        }
        return nil
    }

    var intValue: Int64? {
        if case let .int(i) = self {
            return i
        }
        return nil
    }

    subscript(key: String) -> JSONValue? {
        if case let .object(o) = self {
            return o[key]
        }
        return nil
    }

    /// Decodes this value into a concrete payload type. Used by the SSE
    /// layer to materialize per-kind event payloads on demand.
    func decoded<T: Decodable>(as _: T.Type, using decoder: JSONDecoder = JSONDecoder()) throws -> T {
        let data = try JSONEncoder().encode(self)
        return try decoder.decode(T.self, from: data)
    }

    /// Pretty-printed JSON for display (tool arguments in cards).
    var prettyPrinted: String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(self),
              let s = String(data: data, encoding: .utf8) else { return "" }
        return s
    }
}
