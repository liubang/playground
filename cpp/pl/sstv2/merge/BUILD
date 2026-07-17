# Copyright (c) 2026 The Authors. All rights reserved.

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "merge_iterator",
    srcs = ["merge_iterator.cpp"],
    hdrs = ["merge_iterator.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["@abseil-cpp//absl/status"],
)
