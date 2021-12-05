cc_binary(
    name = "test_murmurhash2",
    srcs = [
        "murmurhash2.h",
        "test_murmurhash2.cc",
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
