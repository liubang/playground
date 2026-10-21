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

#if !MAZE_AXIS_STANDALONE_TESTS
    @testable import Loom
#endif
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

    /// The pre-optimization implementation, retained only for finite-input
    /// equivalence and elapsed/work comparisons. Count every map, including
    /// invisible ticks and the terminating probe; do not clip its scan.
    private func referenceTicks(
        _ axis: MazeAxis, dStart: Double, dEnd: Double,
        maxTicks: Int = 50, pxWidth: Double = 0,
    ) -> (ticks: [MazeTick], probes: Int) {
        let span = dEnd - dStart
        guard span > 0 else { return ([], 0) }
        let steps: [Double] = [1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 43200]
        var step = steps.last!
        for s in steps {
            if span / s > Double(maxTicks) {
                continue
            }
            if pxWidth > 0, (s / span) * pxWidth < MazeAxis.minTickPx {
                continue
            }
            step = s
            break
        }
        var ticks: [MazeTick] = []
        var probes = 0
        for (segS, segE) in axis.segments {
            if segE < dStart || segS > dEnd {
                continue
            }
            var t = (axis.unmap(segS) / step).rounded(.up) * step
            while true {
                let d = axis.map(t)
                probes += 1
                if d > segE + 1e-6 {
                    break
                }
                if d >= dStart - 1e-6, d <= dEnd + 1e-6 {
                    ticks.append(MazeTick(d: d, t: t, label: mazeFormatTick(t)))
                }
                if ticks.count > 200 {
                    return (ticks, probes)
                }
                t += step
            }
        }
        return (ticks.sorted { $0.d < $1.d }, probes)
    }

    func testDeepZoomAtLongSpanTail() {
        for duration in [3600.0, 86400, 604_800] {
            let axis = MazeAxis(ranges: [(0, duration)], tmax: duration)
            let expected = (0 ... 4).map { duration - 4 + Double($0) }
            let ticks = axis.ticks(dStart: duration - 4, dEnd: duration, pxWidth: 800)
            XCTAssertEqual(ticks.map(\.t), expected)
            XCTAssertEqual(ticks.map(\.d), expected)
            let old = referenceTicks(axis, dStart: duration - 4, dEnd: duration, pxWidth: 800)
            XCTAssertEqual(ticks, old.ticks)
            XCTAssertEqual(old.probes, Int(duration) + 2)
            XCTAssertGreaterThan(old.probes, ticks.count * 500)
            // A sub-second viewport still keeps the one-second lattice.
            XCTAssertEqual(axis.ticks(dStart: duration - 0.2, dEnd: duration - 0.1), [])
        }
    }

    func testFoldWindowsAndEpsilonBoundariesMatchReference() {
        let axis = MazeAxis(
            ranges: [(0, 10), (100, 110), (130, 140), (1000, 1020), (10000, 10010)],
            tmax: 10010,
        )
        XCTAssertEqual(axis.gaps.count, 3) // exactly 20 seconds does not fold
        var edges = axis.segments.flatMap { [$0.0, $0.1] }
        edges += axis.gaps.flatMap { [$0.dStart, $0.dEnd] }
        for edge in edges {
            for delta in [-1.1e-6, -1e-6, -0.9e-6, 0, 0.9e-6, 1e-6, 1.1e-6] {
                for width in [0.1, 3, 11, axis.total] {
                    let start = edge + delta
                    XCTAssertEqual(
                        axis.ticks(dStart: start, dEnd: start + width, pxWidth: 800),
                        referenceTicks(axis, dStart: start, dEnd: start + width, pxWidth: 800).ticks,
                        "edge=\(edge) delta=\(delta) width=\(width)",
                    )
                }
            }
        }
        for gap in axis.gaps {
            XCTAssertEqual(axis.ticks(dStart: gap.dStart + 0.01, dEnd: gap.dEnd - 0.01), [])
        }
        // A real tick just after an endpoint can still map within seam epsilon.
        let tail = MazeAxis(ranges: [(0, 9.999999), (100, 110)], tmax: 110)
        let ticks = tail.ticks(dStart: 9, dEnd: 10, pxWidth: 800)
        XCTAssertEqual(ticks.map(\.t), [9, 10])
        XCTAssertEqual(ticks, referenceTicks(tail, dStart: 9, dEnd: 10, pxWidth: 800).ticks)
        // A tiny idle interval can let the old endpoint probe jump into the
        // next span; preserve its duplicate tick and sorted output as well.
        let tiny = MazeAxis(ranges: [(0, 9.9999995), (9.9999998, 12)], tmax: 12)
        let duplicate = tiny.ticks(dStart: 9, dEnd: 11)
        XCTAssertEqual(duplicate.map(\.t), [9, 10, 10, 11])
        XCTAssertEqual(duplicate, referenceTicks(tiny, dStart: 9, dEnd: 11).ticks)
    }

    func testDeterministicFiniteInputsMatchReference() {
        var state: UInt64 = 0x4D41_5A45_4158_4953
        func next(_ limit: Int) -> Int {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Int((state >> 32) % UInt64(limit))
        }
        for sample in 0 ..< 600 {
            var ranges: [(Double, Double)] = []
            var end = 0.0
            for _ in 0 ..< (next(12) + 1) {
                let start = end + Double(next(100)) + Double(next(1000)) / 1000
                end = start + Double(next(180)) + Double(next(1000)) / 1000
                ranges.append(next(2) == 0 ? (start, end) : (end, start))
                if next(4) == 0 {
                    ranges.append((start, max(start, end - 1)))
                }
            }
            let axis = MazeAxis(ranges: Array(ranges.reversed()), tmax: end + 10)
            let start = Double(next(max(1, Int(axis.total + 30)))) - 10 + Double(next(1000)) / 1000
            let finish = start + [0.01, 0.5, 4, 30, 100, 500, 2000][next(7)]
            let cap = [-1, 0, 1, 5, 50, 500][next(6)]
            let width = [0.0, 1, 56, 100, 800, 4000][next(6)]
            XCTAssertEqual(
                axis.ticks(dStart: start, dEnd: finish, maxTicks: cap, pxWidth: width),
                referenceTicks(axis, dStart: start, dEnd: finish, maxTicks: cap, pxWidth: width).ticks,
                "sample=\(sample) start=\(start) end=\(finish) cap=\(cap) px=\(width)",
            )
        }
    }

    func testDenseRandomInputsMatchReference() {
        var state: UInt64 = 0x4445_4E53_4541_5849
        func next(_ limit: Int) -> Int {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Int((state >> 32) % UInt64(limit))
        }
        for sample in 0 ..< 160 {
            var ranges: [(Double, Double)] = []
            // Cluster before, across and just after a tick; include seam
            // epsilon duplicates as well as candidates beyond the last span.
            var end = [0.01, 0.999999, 9.999999, 100.01][next(4)]
            let count = 80 + next(160)
            let unit = sample % 2 == 0 ? 1e-9 : 1e-4
            for _ in 0 ..< count {
                let start = end + Double(1 + next(8)) * unit
                end = start + Double(next(8)) * unit
                ranges.append((start, end))
            }
            let axis = MazeAxis(ranges: ranges, tmax: end)
            XCTAssertEqual(axis.segments.count, count)
            for window in [
                (ranges[0].0, end), (end - 0.05, end + 0.05),
                (0.0, end + 1), (end, end + 1),
            ] {
                let cap = [1, 50, 500][next(3)]
                let px = [0.0, 100, 800][next(3)]
                XCTAssertEqual(
                    axis.ticks(dStart: window.0, dEnd: window.1, maxTicks: cap, pxWidth: px),
                    referenceTicks(
                        axis, dStart: window.0, dEnd: window.1, maxTicks: cap, pxWidth: px,
                    ).ticks,
                    "dense sample=\(sample) count=\(count) window=\(window)",
                )
            }
        }
    }

    private func denseAxis(_ count: Int) -> MazeAxis {
        // Every span intersects the window, every first candidate is t=1,
        // and no tick is emitted: the output cap cannot hide quadratic work.
        let ranges = (0 ..< count).map { i in
            let start = 0.01 + Double(i) * 0.48 / Double(count)
            return (start, start + 0.24 / Double(count))
        }
        return MazeAxis(ranges: ranges, tmax: 0.5)
    }

    func testDenseShortSpansBenchmark() {
        let clock = ContinuousClock()
        // Equal call counts for each size. These batches run long enough to
        // avoid a tiny elapsed sample; compare sizes with a generous 3.5x
        // allowance (N log N is ~2.2x, the former suffix scan was ~4x).
        let iterations = 200
        var elapsed: [Duration] = []
        for count in [5000, 10000] {
            let axis = denseAxis(count)
            XCTAssertEqual(axis.segments.count, count)
            XCTAssertTrue(axis.gaps.isEmpty)
            XCTAssertEqual(axis.ticks(dStart: 0, dEnd: 0.5), [])
            var checksum = 0
            for _ in 0 ..< 5 {
                checksum += axis.ticks(dStart: 0, dEnd: 0.5).count
            }
            let time = clock.measure {
                for _ in 0 ..< iterations {
                    checksum += axis.ticks(dStart: 0, dEnd: 0.5).count
                }
            }
            XCTAssertEqual(checksum, 0)
            elapsed.append(time)
            print(
                "MazeAxis dense benchmark spans=\(count) iterations=\(iterations) "
                    + "optimized=\(time) ticks/call=0 checksum=\(checksum)",
            )
        }
        XCTAssertLessThan(elapsed[1], elapsed[0] * 3.5)

        // Bound old quadratic reference work to the smaller sizes; never run
        // it on the 5000/10000-span scaling batches above.
        let referenceIterations = 12
        for count in [1000, 2000] {
            let axis = denseAxis(count)
            let old = referenceTicks(axis, dStart: 0, dEnd: 0.5)
            XCTAssertEqual(old.ticks, [])
            XCTAssertEqual(old.probes, count)
            XCTAssertEqual(axis.ticks(dStart: 0, dEnd: 0.5), old.ticks)
            var fastChecksum = 0
            var oldChecksum = 0
            let fastTime = clock.measure {
                for _ in 0 ..< referenceIterations {
                    fastChecksum += axis.ticks(dStart: 0, dEnd: 0.5).count
                }
            }
            let oldTime = clock.measure {
                for _ in 0 ..< referenceIterations {
                    let result = referenceTicks(axis, dStart: 0, dEnd: 0.5)
                    oldChecksum += result.probes + result.ticks.count
                }
            }
            XCTAssertEqual(fastChecksum, 0)
            XCTAssertEqual(oldChecksum, count * referenceIterations)
            // Smaller reference batches are informational only; performance
            // gating uses the 200-call 5000/10000-span measurements above.
            print(
                "MazeAxis dense reference spans=\(count) iterations=\(referenceIterations) "
                    + "optimized=\(fastTime) reference=\(oldTime) "
                    + "oldProbes/call=\(old.probes) checksum=\(oldChecksum)",
            )
        }
    }

    func testTickCapAndUnusualInputsTerminate() {
        let axis = MazeAxis(ranges: [(0, 43200 * 500)], tmax: 43200 * 500)
        let ticks = axis.ticks(dStart: 0, dEnd: axis.total, maxTicks: 0)
        XCTAssertEqual(ticks.count, 201)
        XCTAssertEqual(ticks, referenceTicks(axis, dStart: 0, dEnd: axis.total, maxTicks: 0).ticks)
        for window in [
            (Double.nan, 10.0), (0, Double.infinity), (-Double.infinity, 0),
            (-Double.greatestFiniteMagnitude, Double.greatestFiniteMagnitude), (10, 10), (10, 0),
        ] {
            XCTAssertEqual(axis.ticks(dStart: window.0, dEnd: window.1), [])
        }
        let huge = MazeAxis(ranges: [(1e100, 1e100)], tmax: 1e100)
        XCTAssertEqual(huge.ticks(dStart: 1e100.nextDown, dEnd: 1e100.nextUp), [])
        let small = MazeAxis(ranges: [(0, 10)], tmax: 10)
        for px in [Double.nan, .infinity, -.infinity, -1, 0] {
            XCTAssertEqual(
                small.ticks(dStart: 0, dEnd: 10, maxTicks: Int.min, pxWidth: px),
                referenceTicks(small, dStart: 0, dEnd: 10, maxTicks: Int.min, pxWidth: px).ticks,
            )
        }
    }

    func testTailZoomBenchmark() {
        // Print reproducible workload, aggregate elapsed seconds and checksums.
        // Never assert nanosecond timings; the ratio threshold is intentionally
        // far below the removed-work ratio and batches amortize clock overhead.
        let cases: [(String, MazeAxis, Double, Double, Int)] = [
            ("1h-tail", MazeAxis(ranges: [(0, 3600)], tmax: 3600), 3596, 3600, 1000),
            ("7d-tail", MazeAxis(ranges: [(0, 604_800)], tmax: 604_800), 604_796, 604_800, 80),
            (
                "many-folds-tail", MazeAxis(
                    ranges: (0 ..< 48).map { (Double($0) * 7200, Double($0) * 7200 + 3600) },
                    tmax: 47 * 7200 + 3600,
                ), 48 * 3600 + 47 * 3 - 4, 48 * 3600 + 47 * 3, 200,
            ),
        ]
        for (name, axis, start, end, iterations) in cases {
            let old = referenceTicks(axis, dStart: start, dEnd: end, pxWidth: 800)
            XCTAssertEqual(axis.ticks(dStart: start, dEnd: end, pxWidth: 800), old.ticks)
            XCTAssertEqual(old.ticks.count, 5)
            XCTAssertGreaterThanOrEqual(old.probes, 3602)
            var fastChecksum = 0.0
            var oldChecksum = 0.0
            let clock = ContinuousClock()
            // Warm both paths before measuring, and consume every result.
            _ = axis.ticks(dStart: start, dEnd: end, pxWidth: 800)
            _ = referenceTicks(axis, dStart: start, dEnd: end, pxWidth: 800)
            let fastTime = clock.measure {
                for _ in 0 ..< iterations {
                    fastChecksum += axis.ticks(dStart: start, dEnd: end, pxWidth: 800)
                        .reduce(0) { $0 + $1.t }
                }
            }
            let oldTime = clock.measure {
                for _ in 0 ..< iterations {
                    oldChecksum += referenceTicks(axis, dStart: start, dEnd: end, pxWidth: 800)
                        .ticks.reduce(0) { $0 + $1.t }
                }
            }
            XCTAssertEqual(fastChecksum, oldChecksum)
            XCTAssertLessThan(fastTime, oldTime / 2)
            print(
                "MazeAxis benchmark \(name): iterations=\(iterations) oldProbes/call=\(old.probes) "
                    + "ticks/call=\(old.ticks.count) optimized=\(fastTime) "
                    + "reference=\(oldTime) checksum=\(fastChecksum)",
            )
        }
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
