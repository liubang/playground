#!/usr/bin/env python3
"""Run connector scan benchmarks with repeated samples.

Use --build to rebuild benchmark binaries before sampling. Release baselines
should use --build --bazel-config release so the JSON output is not polluted by
fastbuild instrumentation.
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
    "accumulator_input_rows",
    "accumulator_output_rows",
    "accumulator_groups",
    "accumulator_partial_input_rows",
    "accumulator_final_input_rows",
    "accumulator_key_time_ms",
    "accumulator_hash_time_ms",
    "accumulator_update_time_ms",
    "accumulator_result_time_ms",
    "accumulator_partial_time_ms",
    "accumulator_final_time_ms",
]

SQLITE_DEFAULT_SCENARIOS = [
    "scan",
    "filter_project",
    "wide_filter",
    "topn",
    "distinct_host",
    "group_count",
    "group_sum",
    "group_mean",
]
MYSQL_DEFAULT_SCENARIOS = ["scan", "filter_project", "wide_filter", "topn"]


def benchmark_targets(connector):
    if connector == "sqlite":
        return ["//cpp/pl/flux/benchmark:sqlite_scan_benchmark"]
    if connector == "mysql":
        return ["//cpp/pl/flux/benchmark:mysql_scan_benchmark"]
    return [
        "//cpp/pl/flux/benchmark:sqlite_scan_benchmark",
        "//cpp/pl/flux/benchmark:mysql_scan_benchmark",
    ]


def build_benchmarks(args):
    command = ["bazel", "build"]
    if args.bazel_config:
        command.append(f"--config={args.bazel_config}")
    command.extend(benchmark_targets(args.connector))
    subprocess.run(command, cwd=ROOT, check=True)


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
    scenarios = args.scenarios or SQLITE_DEFAULT_SCENARIOS
    results = []
    for scenario in scenarios:
        samples = []
        for run in range(args.warmup):
            db_path = f"/tmp/flux_connector_bench_{scenario}_{os.getpid()}_warmup_{run}.db"
            command = [str(SQLITE_BIN), str(args.sqlite_rows), db_path, scenario]
            if scenario == "wide_filter":
                command.append(str(args.threshold))
            run_json(command)
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
    scenarios = args.scenarios or MYSQL_DEFAULT_SCENARIOS
    env = os.environ.copy()
    env["FLUX_MYSQL_TEST_DSN"] = args.mysql_dsn
    env["FLUX_MYSQL_TARGET_SPLITS"] = str(args.mysql_target_splits)
    env["FLUX_MYSQL_ROWS_PER_PAGE"] = str(args.mysql_rows_per_page)
    env["FLUX_MYSQL_MAX_POOL_SIZE"] = str(args.mysql_max_pool_size)
    env["FLUX_MYSQL_SPLIT_CACHE_MAX_ENTRIES"] = str(args.mysql_split_cache_max_entries)
    env["FLUX_MYSQL_SPLIT_CACHE_TTL_MS"] = str(args.mysql_split_cache_ttl_ms)
    env["FLUX_MYSQL_USE_PREPARED_STATEMENTS"] = (
        "0" if args.mysql_disable_prepared_statements else "1"
    )
    results = []
    for scenario in scenarios:
        samples = []
        for _ in range(args.warmup):
            command = [str(MYSQL_BIN), args.mysql_dsn, args.mysql_table, scenario]
            if scenario == "wide_filter":
                command.append(str(args.threshold))
            run_json(command, env=env)
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
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--sqlite-rows", type=int, default=1_000_000)
    parser.add_argument("--mysql-dsn", default=os.environ.get("FLUX_MYSQL_TEST_DSN", ""))
    parser.add_argument("--mysql-table", default="cpu")
    parser.add_argument("--mysql-rows", type=int, default=1_000_000)
    parser.add_argument("--mysql-target-splits", type=int, default=8)
    parser.add_argument("--mysql-rows-per-page", type=int, default=1024)
    parser.add_argument("--mysql-max-pool-size", type=int, default=8)
    parser.add_argument("--mysql-split-cache-max-entries", type=int, default=1024)
    parser.add_argument("--mysql-split-cache-ttl-ms", type=int, default=300000)
    parser.add_argument("--mysql-disable-prepared-statements", action="store_true")
    parser.add_argument("--prepare-mysql-benchmark-table", action="store_true")
    parser.add_argument("--threshold", type=float, default=50.0)
    parser.add_argument("--scenario", dest="scenarios", action="append")
    parser.add_argument("--build", action="store_true", help="build benchmark targets before running")
    parser.add_argument("--bazel-config", default="release", help="Bazel config used with --build")
    parser.add_argument("--output", help="write the JSON summary to this file as a stable baseline")
    args = parser.parse_args()

    if args.repeat <= 0:
        print("--repeat must be positive", file=sys.stderr)
        return 2
    if args.warmup < 0:
        print("--warmup must not be negative", file=sys.stderr)
        return 2

    if args.build:
        build_benchmarks(args)

    results = []
    if args.connector in ("sqlite", "all"):
        results.extend(run_sqlite(args))
    if args.connector in ("mysql", "all"):
        results.extend(run_mysql(args))

    payload = {"results": results}
    text = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        Path(args.output).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
