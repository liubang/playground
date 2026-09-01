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

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS", "TEST_COPTS")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "core_test",
    srcs = ["ut/core/core_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/core",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "gguf_loader_test",
    srcs = ["ut/loader/gguf_loader_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/loader",
        "//cpp/pl/mllm/ut/testdata:gguf_writer",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "cpu_backend_test",
    srcs = ["ut/backend/cpu_backend_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/backend/cpu:cpu_backend",
        "//cpp/pl/mllm/core",
        "@googletest//:gtest_main",
    ],
)

# Metal backend vs CPU reference parity tests (SPEC §5.2: every kernel needs a
# CPU reference comparison). Objective-C++ test source is compiled by an
# objc_library (cc_test cannot consume .mm directly) and linked into a cc_test.
# Apple-only: builds on macOS, skipped elsewhere (see backend/metal/BUILD).
objc_library(
    name = "metal_backend_test_lib",
    srcs = ["ut/backend/metal_backend_test.mm"],
    copts = TEST_COPTS + [
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
    # manual keeps this objc_library out of `bazel test //...` expansions (same
    # rationale as backend/metal/BUILD); macOS runs it transitively through the
    # non-manual metal_backend_test cc_test.
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:macos"],
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/backend/cpu:cpu_backend",
        "//cpp/pl/mllm/backend/metal:metal_backend",
        "//cpp/pl/mllm/core",
        "@googletest//:gtest_main",
    ],
    # Force-link the archive so the gtest TEST() registration objects (created
    # by static initializers) are pulled into the final cc_test binary.
    alwayslink = True,
)

cc_test(
    name = "metal_backend_test",
    target_compatible_with = ["@platforms//os:macos"],
    deps = [":metal_backend_test_lib"],
)

cc_test(
    name = "kv_cache_test",
    srcs = ["ut/kv_cache/kv_cache_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/model:config",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "model_test",
    srcs = ["ut/model/model_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/backend/cpu:cpu_backend",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/model",
        "//cpp/pl/mllm/model:config",
        "//cpp/pl/mllm/model:dense_decoder",
        "//cpp/pl/mllm/model:transformer_layer",
        "//cpp/pl/mllm/model:weight_names",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "sampler_test",
    srcs = ["ut/sampler/sampler_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/sampler",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "engine_test",
    srcs = ["ut/engine/engine_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/mllm/engine",
        "//cpp/pl/mllm/ut/testdata:gguf_writer",
        "@googletest//:gtest_main",
    ] + select({
        "@platforms//os:macos": ["//cpp/pl/mllm/backend/metal:metal_backend"],
        "//conditions:default": [],
    }),
)
