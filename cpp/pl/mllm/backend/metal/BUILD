# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/08/29 22:15

load("@rules_cc//cc:defs.bzl", "objc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS")

package(default_visibility = ["//visibility:public"])

# Metal backend: Objective-C++ implementation. Metal/Objective-C types are
# confined to the .mm translation unit (SPEC §2.2); metal_backend.h stays pure
# C++ so engine.cpp / cli.cpp can include it. Shaders are compiled at runtime
# from shader_source.h, so no .metal compilation step is required.
#
# Apple-only: `objc_library` requires an Apple toolchain and the Metal /
# MetalPerformanceShaders frameworks exist only on macOS. `bazel build //...`
# on any other platform skips this target entirely.
objc_library(
    name = "metal_backend",
    srcs = ["metal_backend.mm"],
    hdrs = [
        "metal_backend.h",
        "shader_source.h",
    ],
    copts = COPTS + [
        "-std=c++20",
        "-stdlib=libc++",
        "-fobjc-arc",
        "-fblocks",
    ],
    sdk_frameworks = [
        "Foundation",
        "Metal",
        "MetalPerformanceShaders",
    ],
    # `manual` keeps this out of `bazel build //...` expansions entirely: on
    # non-Apple platforms objc_library analysis fails before the compatibility
    # check, so the manual tag is the reliable guard. On macOS the engine still
    # pulls this target in through its select()-gated dependency.
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:macos"],
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/core",
    ],
)
