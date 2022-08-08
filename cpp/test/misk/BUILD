# Copyright (c) 2021 The Authors. All rights reserved.
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
# Created: 2021/12/03 17:14

# vim: ft=bzl

package(default_visibility = ["//visibility:public"])

# [
#     cc_test(
#         name = "%s" % f[0:f.rfind(".")],
#         srcs = [
#             "%s" % f,
#         ] + glob([
#             "include/*.h",
#         ]),
#         copts = [
#             "-std=gnu++17",
#             "-fno-omit-frame-pointer",
#             "-fsanitize=address",
#         ],
#         linkopts = [
#             "-fsanitize=address",
#         ],
#         deps = [
#             "@com_google_googletest//:gtest_main",
#         ],
#     )
#     for f in glob(["*.cc"])
# ]
#

_common_copts = [
    "-fno-omit-frame-pointer",
    "-fsanitize=address",
]

_common_linkopts = [
    "-fsanitize=address",
]

cc_test(
    name = "test_call_once",
    srcs = [
        "test_call_once.cpp",
    ],
    copts = _common_copts + [
        "-std=c++17",
    ],
    linkopts = _common_linkopts + [
        "-pthread",
    ],
    deps = [
        "@gtest//:gtest_main",
    ],
)

cc_test(
    name = "test_detect_class_member",
    srcs = [
        "detect_class_member.h",
        "test_detect_class_member.cpp",
    ],
    copts = _common_copts + [
        "-std=c++11",
    ],
    linkopts = _common_linkopts,
    deps = [
        "@gtest//:gtest_main",
    ],
)

cc_binary(
    name = "test_myvector",
    srcs = [
        "test_myvector.cpp",
    ],
    copts = _common_copts + [
        "-std=c++17",
    ],
    linkopts = _common_linkopts,
)

cc_binary(
    name = "test_myvector1",
    srcs = [
        "test_myvector1.cpp",
    ],
    copts = _common_copts + [
        "-std=c++17",
    ],
    linkopts = _common_linkopts,
)

cc_binary(
    name = "test_union",
    srcs = [
        "test_union.cpp",
    ],
    copts = _common_copts + [
        "-std=c++17",
    ],
    linkopts = _common_linkopts,
)
