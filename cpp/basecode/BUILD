package(default_visibility = ["//visibility:public"])

cc_library(
    name = "basecode",
    srcs = [
        "count_down_latch.cc",
    ] + glob(["logger/*.cc"]),
    hdrs = [
        "cistring.h",
        "singleton.h",
        "count_down_latch.h",
    ] + glob(["logger/*.h"]),
    copts = [
        "-std=c++17",
        "-pthread",
    ],
    deps = [
    ],
)
