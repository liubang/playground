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
        "//basecode/cistring:cistring",
        "@gtest//:gtest_main",
    ],
)
