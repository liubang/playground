# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "counter",
    srcs = ["counter.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minivessel:local_filesystem",
        "//cpp/pl/minivessel:replica_runtime",
        "//cpp/pl/minivessel:shared_wal",
        "//cpp/pl/sstv2/codec:endian",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
