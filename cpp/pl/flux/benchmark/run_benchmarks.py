#!/usr/bin/env python3

import argparse
import json
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


def write_query(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


def run_case(case: str, query_path: Path, flux_bin: Path, timeout_seconds: int) -> dict:
    started = time.time()
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
            "case": case,
            "timeout_s": timeout_seconds,
        }

    return {
        "case": case,
        "rc": proc.returncode,
        "seconds": round(time.time() - started, 3),
        "stdout_lines": len(proc.stdout.splitlines()),
        "stderr_last": proc.stderr.splitlines()[-1] if proc.stderr.splitlines() else "",
    }


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
    args = parser.parse_args()

    flux_bin = Path(args.flux_bin)
    data_dir = Path(args.data_dir)
    work_dir = data_dir
    work_dir.mkdir(parents=True, exist_ok=True)

    results = []

    for rows in (100_000, 500_000, 1_000_000):
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

        results.append(run_case(f"linear:{rows}", linear_query, flux_bin, args.timeout))
        results.append(run_case(f"sort:{rows}", sort_query, flux_bin, args.timeout))
        results.append(run_case(f"agg:{rows}", agg_query, flux_bin, args.timeout))

    for rows in (2_000, 5_000):
        left = data_dir / f"join_left_{rows}.annotated.csv"
        right = data_dir / f"join_right_{rows}.annotated.csv"
        join_query = write_query(
            work_dir / f"join_{rows}.flux",
            JOIN_TEMPLATE.replace("LEFT", f'"{left}"').replace("RIGHT", f'"{right}"'),
        )
        results.append(run_case(f"join:{rows}x{rows}", join_query, flux_bin, args.timeout))

    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
