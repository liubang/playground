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

// MARK: - Folded maze time axis (port of webui/src/components/maze/axis.ts)

//
// Intervals with no step/tool activity for over `gapSecs` (the user
// thinking between turns) collapse into a thin seam labeled ⏸; ticks
// inside activity segments still show true wall-clock seconds. All
// geometry here works in "display seconds" — the domain the canvas
// maps linearly to pixels.

/// A folded idle seam (axis.ts FoldGap), in display-domain coordinates.
struct MazeGap: Equatable, Sendable {
    /// Display-domain left edge of the seam.
    let dStart: Double
    /// Display-domain right edge of the seam.
    let dEnd: Double
    /// Real seconds elided.
    let skipped: Double
}

/// One labeled tick on the axis strip (axis.ts AxisTick).
struct MazeTick: Equatable, Sendable {
    /// Display-domain coordinate.
    let d: Double
    /// Real seconds (label source).
    let t: Double
    let label: String
}

/// Folded axis: real seconds → display seconds with idle gaps removed.
struct MazeAxis: Sendable {
    /// Gaps longer than this many seconds get folded (axis.ts GAP_SECS).
    static let gapSecs = 20.0
    /// Width a folded seam occupies in display-domain seconds (FOLD_TO_SECS).
    static let foldToSecs = 3.0

    private struct Span: Sendable {
        let realStart, dStart, realEnd, dEnd: Double
    }

    private let spans: [Span]
    private let merged: [(Double, Double)]
    private let offset: Double
    private let tmax: Double

    /// Display-domain total length.
    let total: Double
    /// Folded seams (display-domain coordinates).
    let gaps: [MazeGap]
    /// Activity segments (display-domain coordinates, i.e. outside seams).
    let segments: [(Double, Double)]

    /// Build a folded axis from activity ranges (real seconds). Ranges
    /// are merged; inter-range gaps longer than `gapSecs` collapse to
    /// `foldToSecs`.
    init(ranges: [(Double, Double)], tmax: Double) {
        let sorted = ranges
            .filter { $0.0.isFinite && $0.1.isFinite }
            .map { (max(0, min($0.0, $0.1)), max(0, max($0.0, $0.1))) }
            .sorted { $0.0 < $1.0 }
        var merged: [(Double, Double)] = []
        for range in sorted {
            if let last = merged.last, range.0 <= last.1 {
                merged[merged.count - 1].1 = max(last.1, range.1)
            } else {
                merged.append(range)
            }
        }

        var gaps: [MazeGap] = []
        var segments: [(Double, Double)] = []
        var spans: [Span] = []
        var offset = 0.0
        var prevEnd: Double?
        for (s, e) in merged {
            if let prev = prevEnd, s - prev > Self.gapSecs {
                let dStart = offset + prev
                gaps.append(MazeGap(
                    dStart: dStart,
                    dEnd: dStart + Self.foldToSecs,
                    skipped: s - prev - Self.foldToSecs,
                ))
                offset += Self.foldToSecs - (s - prev)
            }
            spans.append(Span(realStart: s, dStart: s + offset, realEnd: e, dEnd: e + offset))
            segments.append((s + offset, e + offset))
            prevEnd = e
        }
        self.merged = merged
        self.spans = spans
        self.offset = offset
        self.tmax = tmax
        self.gaps = gaps
        self.segments = segments
        total = tmax + offset
    }

    /// Real seconds → display seconds.
    func map(_ t: Double) -> Double {
        for i in merged.indices {
            let (s, e) = merged[i]
            if t <= e {
                // Inside (or before) this segment: linear within, clamped at its start.
                return t <= s ? spans[i].dStart : t + (spans[i].dStart - s)
            }
            if i + 1 < merged.count, t < merged[i + 1].0 {
                // Inside a gap: spread linearly across the seam's display width.
                let frac = (t - e) / (merged[i + 1].0 - e)
                return spans[i].dEnd + frac * Self.foldToSecs
            }
        }
        return t + offset
    }

    /// Display seconds → real seconds (clamps to the seam's left edge inside a gap).
    func unmap(_ d: Double) -> Double {
        for span in spans {
            if d >= span.dStart, d <= span.dEnd {
                return span.realStart + (d - span.dStart)
            }
            if d < span.dStart {
                return span.realStart
            }
        }
        return tmax
    }

    private static let tickSteps: [Double] = [1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 43200]

    /// Minimum horizontal pixels between tick marks: labels like "1m34s"
    /// are ~30px at 10px font; tighter spacing makes adjacent labels overlap.
    static let minTickPx = 56.0

    /// Visible ticks for a display-domain window: placed inside activity
    /// segments only, labels carry real wall-clock seconds. pxWidth is the
    /// drawable canvas width; when given, the step also has to leave at
    /// least `minTickPx` between marks.
    func ticks(dStart: Double, dEnd: Double, maxTicks: Int = 50, pxWidth: Double = 0) -> [MazeTick] {
        let span = dEnd - dStart
        guard span > 0 else { return [] }
        // Pick the coarsest tick step (real seconds) that stays within
        // maxTicks AND leaves enough room per label.
        var step = Self.tickSteps[Self.tickSteps.count - 1]
        for s in Self.tickSteps {
            if span / s > Double(maxTicks) {
                continue
            }
            if pxWidth > 0, (s / span) * pxWidth < Self.minTickPx {
                continue
            }
            step = s
            break
        }
        var ticks: [MazeTick] = []
        for (segS, segE) in segments {
            if segE < dStart || segS > dEnd {
                continue
            }
            let realStart = unmap(segS)
            var t = (realStart / step).rounded(.up) * step
            while true {
                let d = map(t)
                if d > segE + 1e-6 {
                    break
                }
                if d >= dStart - 1e-6, d <= dEnd + 1e-6 {
                    ticks.append(MazeTick(d: d, t: t, label: mazeFormatTick(t)))
                }
                if ticks.count > 200 {
                    return ticks
                } // defensive cap
                t += step
            }
        }
        return ticks.sorted { $0.d < $1.d }
    }
}

/// axis.ts formatTick: "34s" / "1m34s" / "2h5m".
func mazeFormatTick(_ t: Double) -> String {
    if t < 60 {
        return "\(Int(t.rounded()))s"
    }
    if t < 3600 {
        let m = Int(t / 60)
        let s = Int(t.truncatingRemainder(dividingBy: 60).rounded())
        return s != 0 ? "\(m)m\(s)s" : "\(m)m"
    }
    let h = Int(t / 3600)
    let m = Int((t.truncatingRemainder(dividingBy: 3600) / 60).rounded())
    return m != 0 ? "\(h)h\(m)m" : "\(h)h"
}

/// axis.ts formatDur (nodes and stats): "340ms" / "1.5s" / "1m34s" / "2h5m".
func mazeFormatDur(_ t: Double) -> String {
    if t < 1 {
        return "\(Int((t * 1000).rounded()))ms"
    }
    if t < 60 {
        let v = (t * 10).rounded() / 10
        return v == v.rounded() ? "\(Int(v))s" : String(format: "%.1fs", v)
    }
    if t < 3600 {
        let m = Int(t / 60)
        let s = Int(t.truncatingRemainder(dividingBy: 60).rounded())
        return s != 0 ? "\(m)m\(s)s" : "\(m)m"
    }
    let h = Int(t / 3600)
    let m = Int((t.truncatingRemainder(dividingBy: 3600) / 60).rounded())
    return m != 0 ? "\(h)h\(m)m" : "\(h)h"
}

/// MazeView.tsx estTextWidth: estimated rendered width of a 10px label —
/// CJK glyphs ~10px, ASCII ~6px.
func mazeEstTextWidth(_ s: String) -> Double {
    s.unicodeScalars.reduce(0) { $0 + ($1.value > 0xFF ? 10 : 6) }
}

/// MazeView.tsx fitLabel: ellipsize a label to fit a pixel budget (estimated).
func mazeFitLabel(_ s: String, budget: Double) -> String {
    if mazeEstTextWidth(s) <= budget {
        return s
    }
    var w = 0.0
    var count = 0
    for scalar in s.unicodeScalars {
        w += scalar.value > 0xFF ? 10 : 6
        if w > budget - 8 {
            break
        }
        count += 1
    }
    return String(String.UnicodeScalarView(s.unicodeScalars.prefix(count))) + "…"
}

/// MazeView.tsx packDetourRows: pack detour spans into rows greedily so
/// detour nodes never overlap on the axis.
func mazePackRows(_ intervals: [(s: Double, e: Double)]) -> [Int] {
    var rowEnds: [Double] = []
    return intervals.map { d in
        for r in rowEnds.indices where rowEnds[r] <= d.s {
            rowEnds[r] = d.e
            return r
        }
        rowEnds.append(d.e)
        return rowEnds.count - 1
    }
}
