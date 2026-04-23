#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/04/19 15:48

import argparse
import json
import statistics
import subprocess
import time
from pathlib import Path


DEFAULT_DATA_DIR = Path("/tmp/flux_bench")
DEFAULT_FLUX_BIN = Path("bazel-bin/cpp/pl/flux/flux")

LINEAR_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> filter(fn: (r) => r.region == "us-east" and r._value > 50.0)
  |> map(fn: (r) => ({r with score: r._value * 1.1}))
  |> limit(n: 1000)
  |> yield(name: "linear")
"""

SORT_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> sort(columns: ["_time", "_value"], desc: true)
  |> limit(n: 1000)
  |> yield(name: "sorted")
"""

AGG_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> group(columns: ["host"])
  |> aggregateWindow(every: 1h, fn: mean, createEmpty: false)
  |> yield(name: "agg")
"""

AGG_CREATE_EMPTY_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> group(columns: ["host"])
  |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-03T00:00:00Z)
  |> aggregateWindow(every: 1h, fn: mean, createEmpty: true)
  |> yield(name: "agg_create_empty")
"""

AGG_CALENDAR_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> group(columns: ["host"])
  |> range(start: 2024-01-01T00:00:00Z, stop: 2024-02-01T00:00:00Z)
  |> aggregateWindow(every: 1mo, fn: mean, createEmpty: false)
  |> yield(name: "agg_calendar")
"""

GROUP_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> group(columns: ["host", "region"])
  |> count()
  |> yield(name: "grouped")
"""

WINDOW_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-02T00:00:00Z)
  |> group(columns: ["host"])
  |> window(every: 1h, createEmpty: true)
  |> count()
  |> yield(name: "windowed")
"""

PIVOT_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> pivot(rowKey: ["host", "region", "_time"], columnKey: ["_field"], valueColumn: "_value")
  |> limit(n: 1000)
  |> yield(name: "pivoted")
"""

PIVOT_WIDE_TEMPLATE = """import "csv"

csv.from(file: DATA)
  |> pivot(rowKey: ["host", "region", "_time"], columnKey: ["_field"], valueColumn: "_value")
  |> yield(name: "pivoted_wide")
"""

ARRAY_TEMPLATE = """import "array"
import "csv"

eastHosts = findColumn(
    tables: csv.from(file: DATA),
    fn: (r) => r.region == "us-east",
    column: "host",
)

taggedHosts = eastHosts
  |> array.map(fn: (host) => ({host: host, cohort: "east"}))
  |> array.filter(fn: (r) => r.host != "")

hostNames = taggedHosts |> array.map(fn: (r) => r.host)
summary = hostNames
  |> array.reduce(
    identity: {count: 0, last: ""},
    fn: (host, accumulator) => ({
      count: accumulator.count + 1,
      last: host,
    }),
  )

{
  containsEdge7: hostNames |> array.contains(value: "edge-7"),
  anyEdge7: hostNames |> array.any(fn: (host) => host == "edge-7"),
  allNamed: hostNames |> array.all(fn: (host) => host != ""),
  count: summary.count,
  last: summary.last,
}
"""

JOIN_TEMPLATE = """import "csv"

join(
    tables: {
        l: csv.from(file: LEFT),
        r: csv.from(file: RIGHT),
    },
    on: ["_time", "host"],
)
    |> limit(n: 1000)
    |> yield(name: "joined")
"""

JOIN_GROUPED_TEMPLATE = """import "csv"

join(
    tables: {
        l: csv.from(file: LEFT) |> group(columns: ["host"]),
        r: csv.from(file: RIGHT) |> group(columns: ["host"]),
    },
    on: ["_time"],
)
    |> limit(n: 1000)
    |> yield(name: "joined_grouped")
"""

JOIN_FULL_TEMPLATE = """import "csv"

join(
    tables: {
        l: csv.from(file: LEFT),
        r: csv.from(file: RIGHT),
    },
    method: "full",
    on: ["_time", "host"],
)
    |> limit(n: 1000)
    |> yield(name: "joined_full")
"""

RANKING_TEMPLATE = """import "csv"

source = csv.from(file: DATA)
  |> group(columns: [])

source |> top(n: 5) |> yield(name: "highest")
source |> bottom(n: 5) |> yield(name: "lowest")
"""


def write_query(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


def run_once(query_path: Path, flux_bin: Path, timeout_seconds: int) -> dict:
    started = time.perf_counter()
    try:
        proc = subprocess.run(
            [str(flux_bin), str(query_path)],
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {
            "timeout_s": timeout_seconds,
        }

    return {
        "rc": proc.returncode,
        "seconds": round(time.perf_counter() - started, 6),
        "stdout_lines": len(proc.stdout.splitlines()),
        "stderr_last": proc.stderr.splitlines()[-1] if proc.stderr.splitlines() else "",
    }


def summarize_samples(samples: list[float]) -> dict:
    return {
        "min_s": round(min(samples), 6),
        "median_s": round(statistics.median(samples), 6),
        "mean_s": round(statistics.fmean(samples), 6),
        "max_s": round(max(samples), 6),
    }


def run_case(
    case: str,
    query_path: Path,
    flux_bin: Path,
    timeout_seconds: int,
    warmup_runs: int,
    repeat_runs: int,
) -> dict:
    for _ in range(warmup_runs):
        warmup = run_once(query_path, flux_bin, timeout_seconds)
        if warmup.get("rc", 0) != 0 or "timeout_s" in warmup:
            return {
                "case": case,
                "warmup_runs": warmup_runs,
                "repeat_runs": repeat_runs,
                **warmup,
            }

    runs = []
    samples = []
    for _ in range(repeat_runs):
        result = run_once(query_path, flux_bin, timeout_seconds)
        if result.get("rc", 0) != 0 or "timeout_s" in result:
            return {
                "case": case,
                "warmup_runs": warmup_runs,
                "repeat_runs": repeat_runs,
                **result,
            }
        runs.append(result)
        samples.append(result["seconds"])

    return {
        "case": case,
        "warmup_runs": warmup_runs,
        "repeat_runs": repeat_runs,
        "samples_s": samples,
        **summarize_samples(samples),
        "stdout_lines": runs[-1]["stdout_lines"],
        "stderr_last": runs[-1]["stderr_last"],
        "rc": runs[-1]["rc"],
    }


def parse_case_filter(raw: str | None) -> set[str] | None:
    if raw is None:
        return None
    names = {part.strip() for part in raw.split(",") if part.strip()}
    return names or None


def selected(case_filter: set[str] | None, name: str) -> bool:
    return case_filter is None or name in case_filter


def parse_int_list(raw: str | None, default_values: tuple[int, ...]) -> tuple[int, ...]:
    if raw is None:
        return default_values
    values = tuple(int(part.strip().replace("_", "")) for part in raw.split(",") if part.strip())
    return values or default_values


def main() -> None:
    parser = argparse.ArgumentParser(description="Run local Flux runtime benchmarks.")
    parser.add_argument(
        "--flux-bin",
        default=str(DEFAULT_FLUX_BIN),
        help="Path to the built flux binary.",
    )
    parser.add_argument(
        "--data-dir",
        default=str(DEFAULT_DATA_DIR),
        help="Directory containing generated benchmark CSV files.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=180,
        help="Per-case timeout in seconds.",
    )
    parser.add_argument(
        "--warmup-runs",
        type=int,
        default=1,
        help="Number of untimed warmup runs per case before sampling.",
    )
    parser.add_argument(
        "--repeat-runs",
        type=int,
        default=5,
        help="Number of timed runs per case used for summary statistics.",
    )
    parser.add_argument(
        "--cases",
        help="Comma-separated case names to run (for example: pivot,pivot_wide,join_grouped).",
    )
    parser.add_argument(
        "--metric-rows",
        help="Comma-separated metric row counts. Defaults to 100000,500000,1000000.",
    )
    parser.add_argument(
        "--join-rows",
        help="Comma-separated join row counts. Defaults to 2000,5000.",
    )
    args = parser.parse_args()

    flux_bin = Path(args.flux_bin)
    data_dir = Path(args.data_dir)
    work_dir = data_dir
    work_dir.mkdir(parents=True, exist_ok=True)
    case_filter = parse_case_filter(args.cases)
    metric_rows = parse_int_list(args.metric_rows, (100_000, 500_000, 1_000_000))
    join_rows = parse_int_list(args.join_rows, (2_000, 5_000))

    results = []

    for rows in metric_rows:
        data = data_dir / f"metrics_{rows}.annotated.csv"
        linear_query = write_query(
            work_dir / f"linear_{rows}.flux",
            LINEAR_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        sort_query = write_query(
            work_dir / f"sort_{rows}.flux",
            SORT_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        agg_query = write_query(
            work_dir / f"agg_{rows}.flux",
            AGG_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        agg_create_empty_query = write_query(
            work_dir / f"agg_create_empty_{rows}.flux",
            AGG_CREATE_EMPTY_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        agg_calendar_query = write_query(
            work_dir / f"agg_calendar_{rows}.flux",
            AGG_CALENDAR_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        group_query = write_query(
            work_dir / f"group_{rows}.flux",
            GROUP_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        window_query = write_query(
            work_dir / f"window_{rows}.flux",
            WINDOW_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        array_query = write_query(
            work_dir / f"array_{rows}.flux",
            ARRAY_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        ranking_query = write_query(
            work_dir / f"ranking_{rows}.flux",
            RANKING_TEMPLATE.replace("DATA", f'"{data}"'),
        )
        pivot_data = data_dir / f"pivot_{rows}.annotated.csv"
        pivot_query = write_query(
            work_dir / f"pivot_{rows}.flux",
            PIVOT_TEMPLATE.replace("DATA", f'"{pivot_data}"'),
        )
        pivot_wide_data = data_dir / f"pivot_wide_{rows}.annotated.csv"
        pivot_wide_query = write_query(
            work_dir / f"pivot_wide_{rows}.flux",
            PIVOT_WIDE_TEMPLATE.replace("DATA", f'"{pivot_wide_data}"'),
        )

        metric_cases = (
            ("linear", linear_query),
            ("sort", sort_query),
            ("agg", agg_query),
            ("agg_create_empty", agg_create_empty_query),
            ("agg_calendar", agg_calendar_query),
            ("group", group_query),
            ("window", window_query),
            ("array", array_query),
            ("ranking", ranking_query),
            ("pivot", pivot_query),
            ("pivot_wide", pivot_wide_query),
        )
        for case_name, query_path in metric_cases:
            if not selected(case_filter, case_name):
                continue
            results.append(
                run_case(
                    f"{case_name}:{rows}",
                    query_path,
                    flux_bin,
                    args.timeout,
                    args.warmup_runs,
                    args.repeat_runs,
                )
            )

    for rows in join_rows:
        left = data_dir / f"join_left_{rows}.annotated.csv"
        right = data_dir / f"join_right_{rows}.annotated.csv"
        join_query = write_query(
            work_dir / f"join_{rows}.flux",
            JOIN_TEMPLATE.replace("LEFT", f'"{left}"').replace("RIGHT", f'"{right}"'),
        )
        join_grouped_query = write_query(
            work_dir / f"join_grouped_{rows}.flux",
            JOIN_GROUPED_TEMPLATE.replace("LEFT", f'"{left}"').replace("RIGHT", f'"{right}"'),
        )
        join_full_query = write_query(
            work_dir / f"join_full_{rows}.flux",
            JOIN_FULL_TEMPLATE.replace("LEFT", f'"{left}"').replace("RIGHT", f'"{right}"'),
        )
        join_cases = (
            ("join", join_query),
            ("join_grouped", join_grouped_query),
            ("join_full", join_full_query),
        )
        for case_name, query_path in join_cases:
            if not selected(case_filter, case_name):
                continue
            results.append(
                run_case(
                    f"{case_name}:{rows}x{rows}",
                    query_path,
                    flux_bin,
                    args.timeout,
                    args.warmup_runs,
                    args.repeat_runs,
                )
            )

    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
