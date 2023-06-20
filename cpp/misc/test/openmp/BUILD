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
# Created: 2023/06/20 00:02

#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/06/19 23:36
# Last Modified: 2023/06/19 23:36
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "common",
    hdrs = glob(["*.h"]),
)

[
    cc_test(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=c++20",
            "-fopenmp",
        ] + TEST_COPTS,
        linkopts = [
            "-fopenmp",
        ] + DEFAULT_LINKOPTS,
        deps = [
            ":common",
            "@com_github_google_benchmark//:benchmark_main",
        ],
    )
    for f in glob(["*.cpp"])
]
