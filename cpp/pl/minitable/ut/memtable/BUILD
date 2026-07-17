# Copyright (c) 2026 The Authors. All rights reserved.

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//cpp:copts/configure_copts.bzl", "LINKOPTS", "TEST_COPTS")

cc_test(
    name = "memtable_test",
    srcs = ["memtable_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minitable/memtable",
        "@googletest//:gtest_main",
    ],
)
