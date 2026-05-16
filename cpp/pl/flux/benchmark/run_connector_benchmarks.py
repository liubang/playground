#!/usr/bin/env python3
"""Run connector scan benchmarks with repeated samples.

The runner intentionally keeps setup simple: build the two benchmark binaries
with Bazel first, then execute stable scenario sets and print JSON summaries.
"""

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse


ROOT = Path(__file__).resolve().parents[4]
SQLITE_BIN = ROOT / "bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark"
MYSQL_BIN = ROOT / "bazel-bin/cpp/pl/flux/benchmark/mysql_scan_benchmark"
PROFILE_KEYS = [
    "split_bytes",
    "split_wall_time_ms",
    "split_metadata_time_ms",
    "split_discovery_time_ms",
    "split_connect_time_ms",
    "split_schema_time_ms",
    "split_sql_build_time_ms",
    "split_execute_time_ms",
    "split_read_time_ms",
    "split_decode_time_ms",
    "split_page_build_time_ms",
]


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
    summary = {
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
    for key in PROFILE_KEYS:
        if key in samples[-1]:
            summary[key] = samples[-1][key]
    return summary


def quote_mysql_identifier(name):
    if not name:
        raise ValueError("mysql identifier must not be empty")
    return "`" + name.replace("`", "``") + "`"


def parse_mysql_url(dsn):
    parsed = urlparse(dsn)
    if parsed.scheme != "mysql" or not parsed.hostname or not parsed.path.strip("/"):
        raise ValueError("mysql dsn must be mysql://user:password@host[:port]/database")
    return {
        "host": parsed.hostname,
        "port": parsed.port or 3306,
        "user": unquote(parsed.username or ""),
        "password": unquote(parsed.password or ""),
        "database": unquote(parsed.path.lstrip("/")),
    }


def digit_generator_sql(rows):
    digits = max(1, len(str(max(rows - 1, 0))))
    sources = [f"(SELECT 0 i UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 "
               f"UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 "
               f"UNION ALL SELECT 8 UNION ALL SELECT 9) d{index}" for index in range(digits)]
    terms = [f"d{index}.i * {10 ** index}" for index in range(digits)]
    return f"SELECT {' + '.join(terms)} AS n FROM {', '.join(sources)}"


def prepare_mysql_benchmark_table(dsn, table, rows):
    mysql = shutil.which("mysql")
    if mysql is None:
        raise RuntimeError("mysql CLI is required for --prepare-mysql-benchmark-table")
    if rows <= 0:
        raise ValueError("--mysql-rows must be positive")

    config = parse_mysql_url(dsn)
    quoted_table = quote_mysql_identifier(table)
    index_name = quote_mysql_identifier(f"{table[:48]}_usage_idx")
    generator = digit_generator_sql(rows)
    sql = f"""
DROP TABLE IF EXISTS {quoted_table};
CREATE TABLE {quoted_table}(
  _time VARCHAR(32) NOT NULL,
  host VARCHAR(32) NOT NULL,
  region VARCHAR(8) NOT NULL,
  `usage` DOUBLE NOT NULL,
  seq BIGINT NOT NULL PRIMARY KEY
);
INSERT INTO {quoted_table}(_time, host, region, `usage`, seq)
SELECT
  CONCAT('2024-07-01T10:', LPAD(MOD(n, 60), 2, '0'), ':00Z') AS _time,
  CONCAT('edge-', MOD(n, 64)) AS host,
  IF(MOD(n, 2) = 0, 'west', 'east') AS region,
  CAST(MOD(n, 100) AS DOUBLE) AS `usage`,
  n AS seq
FROM ({generator}) generated_rows
WHERE n < {int(rows)};
CREATE INDEX {index_name} ON {quoted_table}(`usage`);
SELECT COUNT(*) AS rows_inserted FROM {quoted_table};
"""
    command = [
        mysql,
        "-h",
        config["host"],
        "-P",
        str(config["port"]),
        "-u",
        config["user"],
        "--batch",
        "--raw",
        config["database"],
        "-e",
        sql,
    ]
    env = os.environ.copy()
    env["MYSQL_PWD"] = config["password"]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "mysql benchmark table setup failed:\n"
            + completed.stderr.strip()
            + "\n"
            + completed.stdout.strip()
        )
    return completed.stdout.strip()


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
    if args.prepare_mysql_benchmark_table:
        prepare_mysql_benchmark_table(args.mysql_dsn, args.mysql_table, args.mysql_rows)
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
    parser.add_argument("--mysql-rows", type=int, default=1_000_000)
    parser.add_argument("--prepare-mysql-benchmark-table", action="store_true")
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
