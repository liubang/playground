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

/// Toast ownership (Theme.swift ToastCenter): a toast belongs to the
/// host that was active when it was posted, renders only there, and
/// dies with that host — the settings sheet's toasts must never
/// migrate into the main window when the sheet closes.
@MainActor
final class ToastCenterTests: XCTestCase {
    func testSheetToastDiesWithTheSheet() {
        let center = ToastCenter()
        let window = center.registerHost()
        center.post("window toast")
        let sheet = center.registerHost()
        center.post("sheet toast")

        // While the sheet is up, its toast renders only there.
        XCTAssertEqual(center.items(for: window).map(\.msg), ["window toast"])
        XCTAssertEqual(center.items(for: sheet).map(\.msg), ["sheet toast"])

        // Closing the sheet drops its toast instead of handing it to
        // the window underneath.
        center.unregisterHost(sheet)
        XCTAssertEqual(center.items.map(\.msg), ["window toast"])
        XCTAssertEqual(center.items(for: window).map(\.msg), ["window toast"])
    }

    func testWindowToastSurvivesSheetClose() {
        let center = ToastCenter()
        let window = center.registerHost()
        center.post("window toast")
        let sheet = center.registerHost()
        // The sheet never takes over the window's pending toasts.
        XCTAssertEqual(center.items(for: sheet), [])
        center.unregisterHost(sheet)
        XCTAssertEqual(center.items(for: window).map(\.msg), ["window toast"])
    }

    func testOwnerlessToastFollowsTheActiveHost() {
        let center = ToastCenter()
        // Posted before any host mounted (e.g. during startup).
        center.post("early toast")
        let window = center.registerHost()
        XCTAssertEqual(center.items(for: window).map(\.msg), ["early toast"])
        let sheet = center.registerHost()
        // Only the ACTIVE host renders ownerless toasts — otherwise
        // they would show once per mounted host.
        XCTAssertEqual(center.items(for: window), [])
        XCTAssertEqual(center.items(for: sheet).map(\.msg), ["early toast"])
        center.unregisterHost(sheet)
        XCTAssertEqual(center.items(for: window).map(\.msg), ["early toast"])
    }
}
