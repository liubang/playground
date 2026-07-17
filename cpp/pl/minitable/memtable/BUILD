# Copyright (c) 2026 The Authors. All rights reserved.

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//cpp/pl/minitable:__subpackages__"])

cc_library(
    name = "memtable",
    srcs = ["memtable.cpp"],
    hdrs = ["memtable.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/arena",
        "//cpp/pl/sstv2/merge:merge_iterator",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
