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
# Created: 2026/09/11

load("@rules_cc//cc:defs.bzl", "objc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//visibility:public"])

# Modality-neutral image container + preprocessing primitives.
# Deliberately decode-free: hosts supply raw pixels (ImageIO, ScreenCaptureKit).
cc_library(
    name = "media",
    srcs = ["image.cpp"],
    hdrs = ["image.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["//cpp/pl/mllm/core"],
)

# Image file decoding for host tools: ImageIO on macOS (objc_library, same
# wiring as //cpp/pl/mllm/backend/metal), an unsupported-stub elsewhere.
# Consumers depend on the :image_io alias, which resolves per platform.
alias(
    name = "image_io",
    actual = select({
        "@platforms//os:macos": ":image_io_darwin",
        "//conditions:default": ":image_io_stub",
    }),
)

objc_library(
    name = "image_io_darwin",
    srcs = ["image_io_darwin.mm"],
    hdrs = ["image_io.h"],
    copts = COPTS + [
        "-std=c++20",
        "-stdlib=libc++",
        "-fobjc-arc",
    ],
    sdk_frameworks = [
        "CoreFoundation",
        "CoreGraphics",
        "Foundation",
        "ImageIO",
    ],
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:macos"],
    deps = [
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/media",
    ],
)

cc_library(
    name = "image_io_stub",
    srcs = ["image_io_stub.cpp"],
    hdrs = ["image_io.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/media",
    ],
)
