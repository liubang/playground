load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "harness",
    srcs = [
        "collector.go",
        "env.go",
        "normalize.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/e2e/harness",
    visibility = ["//visibility:public"],
    deps = [
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/client",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/model/replay:replay_lib",
        "//go/pl/loom/internal/runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
        "@in_gopkg_yaml_v3//:yaml_v3",
    ],
)

go_test(
    name = "harness_test",
    srcs = ["normalize_test.go"],
    embed = [":harness"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/runtimeevent",
    ],
)
