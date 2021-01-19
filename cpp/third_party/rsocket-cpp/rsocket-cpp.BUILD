# vim: ft=bzl

_common_copts = [
    "-std=c++14",
    "-Wall",
    "-Wextra",
    "-Wno-noexcept-type",
    "-Wno-unused-parameter",
    "-Wno-weak-vtables",
    "-Wno-padded",
    "-fno-omit-frame-pointer",
    "-momit-leaf-frame-pointer",
    "-fsanitize=address,undefined",
    "-latomic",
]

cc_library(
    name = "yarpl",
    srcs = glob([
        "yarpl/flowable/*.cpp",
        "yarpl/observable/*.cpp",
        "yarpl/single/*.cpp",
        "yarpl/utils/*.cpp",
    ]),
    hdrs = glob([
        "yarpl/**/*.h",
    ]),
    includes = ["."],
    copts = _common_copts,
    deps = [
        "@folly",
        "@glog",
        "@gflags",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "ReactiveSocket",
    srcs = glob([
        "rsocket/transports/tcp/*.cpp",
        "rsocket/statemachine/*.cpp",
        "rsocket/internal/*.cpp",
        "rsocket/framing/*.cpp",
        "rsocket/*.cpp",
    ]),
    hdrs = glob([
        "rsocket/**/*.h",
    ]),
    includes = ["."],
    copts = _common_copts,
    deps = [
        ":yarpl",
    ],
    visibility = ["//visibility:public"],
)
