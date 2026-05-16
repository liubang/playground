#!/usr/bin/env python3
"""Run connector scan benchmarks with repeated samples.

The runner intentionally keeps setup simple: build the two benchmark binaries
with Bazel first, then execute stable scenario sets and print JSON summaries.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
SQLITE_BIN = ROOT / "bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark"
MYSQL_BIN = ROOT / "bazel-bin/cpp/pl/flux/benchmark/mysql_scan_benchmark"


def run_json(command, env=None):
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout.strip())


def summarize(kind, scenario, samples):
    seconds = [item["seconds"] for item in samples]
    output_rows = [item.get("output_rows", 0) for item in samples]
    return {
        "connector": kind,
        "scenario": scenario,
        "samples": samples,
        "samples_s": seconds,
        "median_s": statistics.median(seconds),
        "mean_s": statistics.fmean(seconds),
        "min_s": min(seconds),
        "max_s": max(seconds),
        "output_rows": output_rows[-1] if output_rows else 0,
        "drivers": samples[-1].get("drivers", 0),
    }


def run_sqlite(args):
    scenarios = args.scenarios or ["scan", "filter_project", "wide_filter", "topn"]
    results = []
    for scenario in scenarios:
        samples = []
        for run in range(args.repeat):
            db_path = f"/tmp/flux_connector_bench_{scenario}_{os.getpid()}_{run}.db"
            command = [str(SQLITE_BIN), str(args.sqlite_rows), db_path, scenario]
            if scenario == "wide_filter":
                command.append(str(args.threshold))
            samples.append(run_json(command))
        results.append(summarize("sqlite", scenario, samples))
    return results


def run_mysql(args):
    if not args.mysql_dsn:
        return []
    scenarios = args.scenarios or ["scan", "filter_project", "wide_filter", "topn"]
    env = os.environ.copy()
    env["FLUX_MYSQL_TEST_DSN"] = args.mysql_dsn
    results = []
    for scenario in scenarios:
        samples = []
        for _ in range(args.repeat):
            command = [str(MYSQL_BIN), args.mysql_dsn, args.mysql_table, scenario]
            if scenario == "wide_filter":
                command.append(str(args.threshold))
            samples.append(run_json(command, env=env))
        results.append(summarize("mysql", scenario, samples))
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--connector", choices=["sqlite", "mysql", "all"], default="sqlite")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--sqlite-rows", type=int, default=1_000_000)
    parser.add_argument("--mysql-dsn", default=os.environ.get("FLUX_MYSQL_TEST_DSN", ""))
    parser.add_argument("--mysql-table", default="cpu")
    parser.add_argument("--threshold", type=float, default=50.0)
    parser.add_argument("--scenario", dest="scenarios", action="append")
    args = parser.parse_args()

    if args.repeat <= 0:
        print("--repeat must be positive", file=sys.stderr)
        return 2

    results = []
    if args.connector in ("sqlite", "all"):
        results.extend(run_sqlite(args))
    if args.connector in ("mysql", "all"):
        results.extend(run_mysql(args))

    print(json.dumps({"results": results}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
