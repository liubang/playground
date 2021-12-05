cc_library(
    name = "bloom_filter",
    srcs = [
        "bloom_filter.cc",
    ],
    hdrs = [
        "bloom_filter.h",
    ],
    copts = [
        "-std=gnu++17",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//highkyck/hash",
    ],
)

cc_binary(
    name = "test_bloom_filter",
    srcs = [
        "test_bloom_filter.cc",
    ],
    deps = [
        ":bloom_filter",
    ],
)
