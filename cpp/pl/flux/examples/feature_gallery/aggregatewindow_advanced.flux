import "csv"

cpu = csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu" and r.host == "edge-1")

cpu
    |> aggregateWindow(
        every: 30s,
        fn: mean,
        offset: 10s,
        timeSrc: "_start",
        timeDst: "bucket_time",
        createEmpty: false,
    )
    |> yield(name: "fixed_offset_windows")

from(
    bucket: "spread",
    rows: [
        {_time: "2024-06-01T09:00:00Z", load: 12.0},
        {_time: "2024-06-01T09:00:10Z", load: 16.0},
        {_time: "2024-06-01T09:00:20Z", load: 18.0},
        {_time: "2024-06-01T09:00:30Z", load: 13.0},
        {_time: "2024-06-01T09:00:40Z", load: 19.0},
        {_time: "2024-06-01T09:00:50Z", load: 21.0},
    ],
)
    |> range(start: 2024-06-01T09:00:00Z, stop: 2024-06-01T09:01:00Z)
    |> aggregateWindow(
        every: 20s,
        period: 40s,
        column: "load",
        fn: (values) => max(values) - min(values),
        createEmpty: false,
    )
    |> yield(name: "spread_overlap_windows")

from(
    bucket: "lookback",
    rows: [
        {_time: "2024-06-01T09:00:00Z", _value: 2.0},
        {_time: "2024-06-01T09:00:10Z", _value: 4.0},
        {_time: "2024-06-01T09:00:20Z", _value: 6.0},
        {_time: "2024-06-01T09:00:30Z", _value: 8.0},
        {_time: "2024-06-01T09:00:40Z", _value: 10.0},
        {_time: "2024-06-01T09:00:50Z", _value: 12.0},
    ],
)
    |> range(start: 2024-06-01T09:00:00Z, stop: 2024-06-01T09:01:00Z)
    |> aggregateWindow(every: 20s, period: "-40s", fn: count, createEmpty: false)
    |> yield(name: "lookback_windows")

from(
    bucket: "monthly",
    rows: [
        {
            _time: "2024-03-15T12:00:00Z",
            _value: 10.0,
            host: "edge-1",
            region: "us-west",
            note: "drop-me",
        },
    ],
)
    |> group(columns: ["host"])
    |> aggregateWindow(
        every: 1mo,
        fn: mean,
        location: {zone: "America/Los_Angeles", offset: 0s},
        timeSrc: "_start",
        timeDst: "bucket_time",
    )
    |> yield(name: "calendar_tz_shape")

from(
    bucket: "monthly-offset",
    rows: [
        {_time: "2024-01-20T00:00:00Z", _value: 10.0},
        {_time: "2024-02-20T00:00:00Z", _value: 30.0},
    ],
)
    |> aggregateWindow(every: 1mo, offset: 15d, fn: mean, createEmpty: false)
    |> yield(name: "calendar_offset_windows")

from(
    bucket: "selector",
    rows: [
        {_time: "2024-01-01T00:00:10Z", _value: 10.0},
        {_time: "2024-01-01T00:02:05Z", _value: 30.0},
    ],
)
    |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
    |> aggregateWindow(every: 1m, fn: last, createEmpty: true)
    |> yield(name: "selector_sparse_windows")
