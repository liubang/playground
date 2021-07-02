cc_binary(
    name = "test_murmurhash2",
    srcs = [
        "test_murmurhash2.cc",
        "murmurhash2.h",
    ],
    copts = [
        "-std=gnu++17",
    ],
)

cc_library(
    name = "hash",
    hdrs = [
        "murmurhash2.h",
    ],
    visibility = ["//visibility:public"],
)
