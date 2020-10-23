load("//:copts/configure_copts.bzl", "DEFAULT_COPTS")

_copts = [
    "-std=gnu++14",
]

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
    copts = _copts,
    deps = [
        "@folly",
    ],
)

cc_binary(
    name = "singleton",
    srcs = [
        "singleton.cc",
        "singleton.h",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)
