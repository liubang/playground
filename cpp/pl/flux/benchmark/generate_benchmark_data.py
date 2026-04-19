#!/usr/bin/env python3

from pathlib import Path


OUTPUT_DIR = Path("/tmp/flux_bench")
METRIC_ROW_COUNTS = (100_000, 500_000, 1_000_000)
JOIN_ROW_COUNTS = (2_000, 5_000)
REGIONS = ("us-east", "us-west", "eu-central", "ap-south")


def metric_timestamp(index: int) -> str:
    minute = index % 60
    hour = (index // 60) % 24
    day = 1 + ((index // (60 * 24)) % 28)
    return f"2024-01-{day:02d}T{hour:02d}:{minute:02d}:00Z"


def host_name(index: int) -> str:
    return f"edge-{(index % 20) + 1}"


def write_metric_file(path: Path, rows: int) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write("#datatype,string,long,dateTime:RFC3339,string,string,double\n")
        handle.write("#group,false,false,false,false,false,false\n")
        handle.write("#default,_result,,,,,\n")
        handle.write(",result,table,_time,host,region,_value\n")
        for index in range(rows):
            handle.write(
                f",,0,{metric_timestamp(index)},{host_name(index)},"
                f"{REGIONS[index % len(REGIONS)]},{(index % 1000) / 10.0:.1f}\n"
            )


def write_join_file(path: Path, rows: int, divisor: float) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write("#datatype,string,long,dateTime:RFC3339,string,double\n")
        handle.write("#group,false,false,false,false,false\n")
        handle.write("#default,_result,,,,\n")
        handle.write(",result,table,_time,host,_value\n")
        for index in range(rows):
            handle.write(
                f",,0,{metric_timestamp(index)},{host_name(index)},"
                f"{(index % 1000) / divisor:.1f}\n"
            )


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    for rows in METRIC_ROW_COUNTS:
        write_metric_file(OUTPUT_DIR / f"metrics_{rows}.annotated.csv", rows)

    for rows in JOIN_ROW_COUNTS:
        write_join_file(OUTPUT_DIR / f"join_left_{rows}.annotated.csv", rows, 10.0)
        write_join_file(OUTPUT_DIR / f"join_right_{rows}.annotated.csv", rows, 20.0)

    print(f"Generated benchmark inputs in {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
