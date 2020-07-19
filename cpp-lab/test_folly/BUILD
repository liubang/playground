load("//:copts/configure_copts.bzl", "DEFAULT_COPTS")

cc_binary(
    name = "scope",
    srcs = [
        "scope.cc",
    ],
    copts = DEFAULT_COPTS,
    deps = [
        "@folly",
    ],
)
cc_binary(
    name = "future",
    srcs = [
        "future.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
    deps = [
        "@folly",
    ],
)
