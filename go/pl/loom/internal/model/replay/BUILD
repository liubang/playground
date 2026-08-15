load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "replay_lib",
    srcs = [
        "binding.go",
        "fingerprint.go",
        "record.go",
        "replay.go",
        "script.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/model/replay",
    visibility = ["//visibility:public"],
    deps = ["//go/pl/loom/internal/domain:domain_lib"],
)

go_test(
    name = "replay_test",
    srcs = ["replay_test.go"],
    embed = [":replay_lib"],
    deps = ["//go/pl/loom/internal/domain:domain_lib"],
)
