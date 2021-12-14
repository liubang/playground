package(default_visibility = ["//visibility:public"])

cc_library(
    name = "basecode",
    srcs = [
        "count_down_latch.cc",
        "crc64.cc",
    ] + glob(["logger/*.cc"]),
    hdrs = [
        "cistring.h",
        "singleton.h",
        "count_down_latch.h",
        "crc64.h",
    ] + glob(["logger/*.h"]),
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-pthread",
    ],
)
