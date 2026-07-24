load("@rules_go//go:def.bzl", "go_library", "go_test")
go_library(
    name = "runtimeevent",
    srcs = [
        "aggregator.go",
        "broker.go",
        "event.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/runtimeevent",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
    ],
)

go_test(
    name = "runtimeevent_test",
    srcs = [
        "aggregator_test.go",
        "broker_test.go",
        "event_test.go",
    ],
    embed = [":runtimeevent"],
    deps = ["//go/pl/loom/internal/domain:domain_lib"],
)
