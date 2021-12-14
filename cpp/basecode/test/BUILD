# vim: ft=bzl
package(default_visibility = ["//visibility:public"])

cc_test(
    name = "test_cistring",
    srcs = [
        "test_cistring.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "//basecode",
        "@gtest//:gtest_main",
    ],
)

cc_test(
    name = "test_crc64",
    srcs = [
        "test_crc64.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "//basecode",
        "@gtest//:gtest_main",
    ],
)
