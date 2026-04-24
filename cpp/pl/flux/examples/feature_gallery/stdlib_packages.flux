import "csv"
import "math"
import "regexp"
import "strings"

option alerting = {
    name: "stdlib-packages",
    hotCpu: 80.0,
    hostPattern: regexp.compile(v: "edge-[13]"),
}

serviceLabel = (r) => strings.toUpper(v: strings.trimSpace(v: r.service))

csv.from(file: "cpp/pl/flux/examples/feature_gallery/data/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu")
    |> map(fn: (r) => ({
        r with
        service_label: serviceLabel(r: r),
        node_alias: strings.replaceAll(v: r.host, t: "edge", u: "node"),
        local_region: strings.hasPrefix(v: r.region, prefix: "us-"),
        watched_host: regexp.matchRegexpString(r: alerting.hostPattern, v: r.host),
        rounded_cpu: math.round(x: r._value),
        cpu_gap: math.abs(x: r._value - alerting.hotCpu),
        load_score: math.sqrt(x: math.pow(x: r._value, y: 2.0)),
    }))
    |> filter(fn: (r) => r.watched_host or r.local_region)
    |> keep(
        columns: [
            "_time",
            "host",
            "region",
            "service_label",
            "node_alias",
            "_value",
            "rounded_cpu",
            "cpu_gap",
            "load_score",
            "watched_host",
            "local_region",
        ],
    )
    |> sort(columns: ["host", "_time"])
    |> yield(name: "stdlib_service_health")

csv.from(file: "cpp/pl/flux/examples/feature_gallery/data/site_ops.annotated.csv")
    |> filter(
        fn: (r) =>
            r._measurement == "cpu" and
            strings.containsStr(v: r.status, substr: "warn") and
            regexp.matchRegexpString(r: /edge-.*/, v: r.host),
    )
    |> map(fn: (r) => ({
        r with
        incident_key: strings.toUpper(v: r.service) + "-" + regexp.quoteMeta(v: r.host),
        severity: (if r._value >= alerting.hotCpu then "critical" else "watch"),
        above_target: math.ceil(x: math.abs(x: r._value - alerting.hotCpu)),
    }))
    |> keep(columns: ["_time", "host", "service", "_value", "incident_key", "severity", "above_target"])
    |> yield(name: "stdlib_alert_candidates")
