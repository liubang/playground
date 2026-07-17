# Copyright (c) 2026 The Authors. All rights reserved.

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "buffer",
    hdrs = [
        "buffer_reader.h",
        "buffer_writer.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
