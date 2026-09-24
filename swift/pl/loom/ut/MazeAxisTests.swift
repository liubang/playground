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

/// MazeAxis is a port of webui/src/components/maze/axis.ts — these cases
/// pin the folding, mapping, tick placement, and label formats so the
/// desktop maze stays pixel-faithful to the WebUI.
final class MazeAxisTests: XCTestCase {
    func testIdentityWhenNoGaps() {
        let axis = MazeAxis(ranges: [(0, 10), (5, 15)], tmax: 15)
        XCTAssertEqual(axis.total, 15, accuracy: 1e-9)
        XCTAssertTrue(axis.gaps.isEmpty)
        XCTAssertEqual(axis.segments.count, 1)
        XCTAssertEqual(axis.map(7), 7, accuracy: 1e-9)
        XCTAssertEqual(axis.unmap(7), 7, accuracy: 1e-9)
    }

    func testLongIdleGapFoldsIntoSeam() {
        let axis = MazeAxis(ranges: [(0, 10), (100, 110)], tmax: 110)
        // 90s of idleness collapses into a 3-display-second seam.
        XCTAssertEqual(axis.gaps.count, 1)
        let gap = axis.gaps[0]
        XCTAssertEqual(gap.dStart, 10, accuracy: 1e-9)
        XCTAssertEqual(gap.dEnd, 13, accuracy: 1e-9)
        XCTAssertEqual(gap.skipped, 87, accuracy: 1e-9)
        XCTAssertEqual(axis.total, 23, accuracy: 1e-9)

        // Real → display across the seam.
        XCTAssertEqual(axis.map(10), 10, accuracy: 1e-9)
        XCTAssertEqual(axis.map(100), 13, accuracy: 1e-9)
        XCTAssertEqual(axis.map(105), 18, accuracy: 1e-9)
        // Inside the gap: spread linearly across the seam's width.
        XCTAssertEqual(axis.map(55), 11.5, accuracy: 1e-9)
        // Display → real: inside a segment shifts back; in the seam it
        // clamps to the seam's right edge (next segment's real start).
        XCTAssertEqual(axis.unmap(16), 103, accuracy: 1e-9)
        XCTAssertEqual(axis.unmap(11), 100, accuracy: 1e-9)
    }

    func testGapBelowThresholdStaysUnfolded() {
        let axis = MazeAxis(ranges: [(0, 10), (25, 30)], tmax: 30) // 15s idle
        XCTAssertTrue(axis.gaps.isEmpty)
        XCTAssertEqual(axis.total, 30, accuracy: 1e-9)
        // Mapping is only defined in-domain (activity segments): no node,
        // tool span, or tick ever sits inside an idle stretch.
        XCTAssertEqual(axis.map(5), 5, accuracy: 1e-9)
        XCTAssertEqual(axis.map(27), 27, accuracy: 1e-9)
        XCTAssertEqual(axis.unmap(27), 27, accuracy: 1e-9)
    }

    func testBeyondLastSegmentMapsWithOffset() {
        let axis = MazeAxis(ranges: [(0, 10), (100, 110)], tmax: 120)
        XCTAssertEqual(axis.map(115), 28, accuracy: 1e-9)
        XCTAssertEqual(axis.unmap(28), 120, accuracy: 1e-9) // past the last span clamps to tmax
    }

    func testTicksStayInsideActivitySegments() {
        let axis = MazeAxis(ranges: [(0, 10), (100, 110)], tmax: 110)
        let ticks = axis.ticks(dStart: 0, dEnd: axis.total, pxWidth: 800)
        XCTAssertFalse(ticks.isEmpty)
        // Step 2 (span 23): ticks at even real seconds inside both
        // segments; nothing lands inside the folded seam (10, 13).
        XCTAssertTrue(ticks.allSatisfy { $0.d <= 10 + 1e-9 || $0.d >= 13 - 1e-9 })
        XCTAssertEqual(ticks.first?.label, "0s")
        XCTAssertEqual(ticks.last?.label, "1m50s")
    }

    func testTickStepRespectsPixelBudget() {
        let axis = MazeAxis(ranges: [(0, 120)], tmax: 120)
        // 100px for 120s: only a 120s step leaves ≥56px between marks.
        let ticks = axis.ticks(dStart: 0, dEnd: 120, pxWidth: 100)
        XCTAssertEqual(ticks.map(\.label), ["0s", "2m"])
    }

    func testTicksRespectWindow() {
        let axis = MazeAxis(ranges: [(0, 60)], tmax: 60)
        let ticks = axis.ticks(dStart: 20, dEnd: 40, pxWidth: 800)
        XCTAssertTrue(ticks.allSatisfy { $0.d >= 20 - 1e-6 && $0.d <= 40 + 1e-6 })
        XCTAssertTrue(ticks.contains { $0.label == "30s" })
    }

    func testFormats() {
        XCTAssertEqual(mazeFormatTick(34), "34s")
        XCTAssertEqual(mazeFormatTick(94), "1m34s")
        XCTAssertEqual(mazeFormatTick(120), "2m")
        XCTAssertEqual(mazeFormatTick(3660), "1h1m")
        XCTAssertEqual(mazeFormatTick(7200), "2h")

        XCTAssertEqual(mazeFormatDur(0.34), "340ms")
        XCTAssertEqual(mazeFormatDur(1.54), "1.5s")
        XCTAssertEqual(mazeFormatDur(1.56), "1.6s")
        XCTAssertEqual(mazeFormatDur(12), "12s")
        XCTAssertEqual(mazeFormatDur(94), "1m34s")
        XCTAssertEqual(mazeFormatDur(3600), "1h")
        XCTAssertEqual(mazeFormatDur(7500), "2h5m")
    }

    func testEstTextWidthAndFitLabel() {
        XCTAssertEqual(mazeEstTextWidth("abc"), 18)
        XCTAssertEqual(mazeEstTextWidth("你好"), 20)
        XCTAssertEqual(mazeFitLabel("abcdefgh", budget: 30), "abc…")
        XCTAssertEqual(mazeFitLabel("abc", budget: 30), "abc") // fits: untouched
    }

    func testPackDetourRowsGreedilyAvoidsOverlap() {
        let rows = mazePackRows([(s: 0, e: 5), (s: 1, e: 2), (s: 6, e: 8), (s: 2, e: 9)])
        XCTAssertEqual(rows, [0, 1, 0, 1])
        XCTAssertEqual(mazePackRows([]), [])
    }
}
