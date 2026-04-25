import "csv"
import "date"

cpu = csv.from(file: "cpp/pl/flux/examples/feature_gallery/data/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu")
    |> map(fn: (r) => ({
        r with
        year: date.year(t: r._time),
        month: date.month(t: r._time),
        month_day: date.monthDay(t: r._time),
        weekday: date.weekDay(t: r._time),
        hour: date.hour(t: r._time),
        minute: date.minute(t: r._time),
        second: date.second(t: r._time),
        hour_start: date.truncate(t: r._time, unit: 1h),
        review_by: date.add(d: 30m, to: r._time),
        lookback_start: date.sub(d: 15m, from: r._time),
    }))

cpu
    |> keep(
        columns: [
            "_time",
            "host",
            "service",
            "_value",
            "year",
            "month",
            "month_day",
            "weekday",
            "hour",
            "minute",
            "second",
            "hour_start",
            "review_by",
            "lookback_start",
        ],
    )
    |> sort(columns: ["host", "_time"])
    |> yield(name: "date_calendar_shape")

cpu
    |> group(columns: ["weekday", "service"])
    |> count(column: "_value")
    |> keep(columns: ["weekday", "service", "_value"])
    |> yield(name: "date_weekday_service_counts")
