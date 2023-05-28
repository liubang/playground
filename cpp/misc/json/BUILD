# Copyright (c) 2023 The Authors. All rights reserved.
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
# Created: 2023/05/29 00:13

#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/28 21:00
# Last Modified: 2023/05/28 21:00
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_library(
    name = "json",
    hdrs = ["json.h"],
    copts = ["-std=c++20"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "json_test",
    srcs = ["json_test.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        ":json",
        "@googletest//:gtest_main",
    ],
)
