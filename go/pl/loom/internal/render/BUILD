load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "render",
    srcs = [
        "diff.go",
        "jsonl.go",
        "linear.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/render",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
    ],
)

go_test(
    name = "render_test",
    srcs = [
        "diff_test.go",
        "jsonl_test.go",
        "linear_test.go",
    ],
    embed = [":render"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
        "@com_github_stretchr_testify//assert",
        "@com_github_stretchr_testify//require",
    ],
)
