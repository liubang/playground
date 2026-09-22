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

/// Fill/collect round-trip semantics for the settings panel's spec
/// engine (ports of the WebUI settings/convert.ts + cfgpath.ts rules):
/// blank = key not written; every field type maps config values to raw
/// control state and back.
final class SettingsConvertTests: XCTestCase {
    // MARK: cfgpath

    func testGetSetPath() {
        var obj: [String: JSONValue] = [:]
        setPath(&obj, "approval.mode", .string("never"))
        XCTAssertEqual(getPath(obj, "approval.mode"), .string("never"))
        // Setting a sibling keeps the existing branch.
        setPath(&obj, "approval.extra", .int(1))
        XCTAssertEqual(getPath(obj, "approval.mode"), .string("never"))
        XCTAssertEqual(getPath(obj, "approval.extra"), .int(1))
        XCTAssertNil(getPath(obj, "approval.missing"))
        // Overwriting a scalar branch with an object path.
        setPath(&obj, "approval.mode.deep", .bool(true))
        XCTAssertEqual(getPath(obj, "approval.mode.deep"), .bool(true))
    }

    func testPreserveUnmanaged() {
        var orig: [String: JSONValue] = [:]
        setPath(&orig, "ui.keymap", .string("vim"))
        setPath(&orig, "skills.disabled", .array([.string("x")]))
        orig["future_section"] = .object(["a": .int(1)])
        orig["approval"] = .object(["mode": .string("never")])

        var cfg: [String: JSONValue] = [:]
        preserveUnmanaged(&cfg, orig: orig)
        // Unknown top-level keys and PRESERVE_PATHS ride back verbatim.
        XCTAssertEqual(cfg["future_section"], .object(["a": .int(1)]))
        XCTAssertEqual(getPath(cfg, "ui.keymap"), .string("vim"))
        XCTAssertEqual(getPath(cfg, "skills.disabled"), .array([.string("x")]))
        // Known managed keys are NOT preserved (the form owns them).
        XCTAssertNil(cfg["approval"])
    }

    // MARK: fillValue

    func testFillScalars() {
        XCTAssertEqual(fillValue(FieldSpec("k"), .string("v")), .text("v"))
        XCTAssertEqual(fillValue(FieldSpec("k"), .int(5)), .text("5"))
        XCTAssertEqual(fillValue(FieldSpec("k"), .double(0.95)), .text("0.95"))
        XCTAssertEqual(fillValue(FieldSpec("k"), nil), .text(""))
        // Integral doubles print without the fraction (JS String(n) parity).
        XCTAssertEqual(fillValue(FieldSpec("k"), .double(5)), .text("5"))
    }

    func testFillBoolAndTristate() {
        let boolSpec = FieldSpec("k", type: .bool)
        XCTAssertEqual(fillValue(boolSpec, .bool(true)), .flag(true))
        XCTAssertEqual(fillValue(boolSpec, nil), .flag(false))

        let triSpec = FieldSpec("k", type: .tristate)
        XCTAssertEqual(fillValue(triSpec, .bool(true)), .text("true"))
        XCTAssertEqual(fillValue(triSpec, .bool(false)), .text("false"))
        XCTAssertEqual(fillValue(triSpec, nil), .text(""))
    }

    func testFillLists() {
        let listSpec = FieldSpec("k", type: .listText)
        XCTAssertEqual(
            fillValue(listSpec, .array([.string("a"), .string("b")])),
            .text("a\nb"),
        )

        let pairSpec = FieldSpec("k", type: .pairList)
        XCTAssertEqual(
            fillValue(pairSpec, .array([
                .object(["name": .string("kb"), "description": .string("知识库")]),
                .object(["name": .string("solo")]),
            ])),
            .text("kb: 知识库\nsolo"),
        )

        let kvSpec = FieldSpec("k", type: .kvText)
        XCTAssertEqual(
            fillValue(kvSpec, .object(["A": .string("1"), "B": .string("2")])),
            .text("A=1\nB=2"),
        )

        let floatSpec = FieldSpec("k", type: .floatList)
        XCTAssertEqual(
            fillValue(floatSpec, .array([.double(0.6), .double(0.75)])),
            .text("0.6, 0.75"),
        )

        let flagSpec = FieldSpec("k", type: .flagList, flagValue: ["text", "image"])
        XCTAssertEqual(fillValue(flagSpec, .array([.string("text")])), .flag(true))
        XCTAssertEqual(fillValue(flagSpec, nil), .flag(false))
    }

    // MARK: collectValue

    private func collect(_ spec: FieldSpec, _ state: ControlState) -> [String: JSONValue] {
        var obj: [String: JSONValue] = [:]
        collectValue(spec, state, into: &obj)
        return obj
    }

    func testCollectEmptyWritesNothing() {
        XCTAssertTrue(collect(FieldSpec("a.b"), .text("  ")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .number), .text("")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .password), .text("")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .select), .text("")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .tristate), .text("")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .listText), .text("\n \n")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .kvText), .text("=v\nk")).isEmpty)
        XCTAssertTrue(collect(FieldSpec("a.b", type: .bool), .flag(false)).isEmpty)
    }

    func testCollectScalars() {
        XCTAssertEqual(
            collect(FieldSpec("approval.mode", type: .select), .text("never")),
            ["approval": .object(["mode": .string("never")])],
        )
        XCTAssertEqual(
            collect(FieldSpec("limits.max_tokens", type: .number), .text("42")),
            ["limits": .object(["max_tokens": .int(42)])],
        )
        XCTAssertEqual(
            collect(FieldSpec("context.utilization", type: .number), .text("0.95")),
            ["context": .object(["utilization": .double(0.95)])],
        )
        XCTAssertEqual(
            collect(FieldSpec("rules.enabled", type: .tristate), .text("false")),
            ["rules": .object(["enabled": .bool(false)])],
        )
        // Secrets are not trimmed.
        XCTAssertEqual(
            collect(FieldSpec("tracing.secret_key", type: .password), .text(" sk ")),
            ["tracing": .object(["secret_key": .string(" sk ")])],
        )
    }

    func testCollectStructured() {
        XCTAssertEqual(
            collect(FieldSpec("skills.extra_roots", type: .listText), .text(" ~/a \n\n~/b")),
            ["skills": .object(["extra_roots": .array([.string("~/a"), .string("~/b")])])],
        )
        XCTAssertEqual(
            collect(FieldSpec("mcp.env", type: .kvText), .text("A=1\nB=x=y\nbad")),
            ["mcp": .object(["env": .object(["A": .string("1"), "B": .string("x=y")])])],
        )
        XCTAssertEqual(
            collect(FieldSpec("kb.collections", type: .pairList), .text("kb: 描述\nsolo")),
            ["kb": .object(["collections": .array([
                .object(["name": .string("kb"), "description": .string("描述")]),
                .object(["name": .string("solo")]),
            ])])],
        )
        XCTAssertEqual(
            collect(FieldSpec("context.notice_levels", type: .floatList), .text("0.6, 0.75 junk")),
            ["context": .object(["notice_levels": .array([.double(0.6), .double(0.75)])])],
        )
        // flag-list checked writes the fixed array verbatim.
        XCTAssertEqual(
            collect(FieldSpec("modalities", type: .flagList, flagValue: ["text", "image"]), .flag(true)),
            ["modalities": .array([.string("text"), .string("image")])],
        )
    }

    // MARK: Round trip

    func testRoundTrip() {
        var orig: [String: JSONValue] = [:]
        setPath(&orig, "limits.max_tokens", .int(1000))
        setPath(&orig, "skills.extra_roots", .array([.string("~/a")]))
        setPath(&orig, "rules.enabled", .bool(true))

        let specs = [
            FieldSpec("limits.max_tokens", type: .number),
            FieldSpec("skills.extra_roots", type: .listText),
            FieldSpec("rules.enabled", type: .tristate),
        ]
        var states: [String: ControlState] = [:]
        for spec in specs {
            states[spec.key] = fillValue(spec, getPath(orig, spec.key))
        }
        var out: [String: JSONValue] = [:]
        collectFields(specs, states, into: &out)
        XCTAssertEqual(out, orig)
    }
}
