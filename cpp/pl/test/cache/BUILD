# Copyright (c) 2024 The Authors. All rights reserved.
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

load(
    "//cpp:copts/configure_copts.bzl",
    "LINKOPTS_WITH_ASAN",
    "TEST_COPTS",
)

[
    cc_test(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=c++20",
            "-ffast-math",
        ] + TEST_COPTS + select({
            "//:linux_x86_64": [
                "-fopenmp",
                "-march=native",
            ],
            "//:darwin": ["-mcpu=apple-m1"],
            "//conditions:default": [],
        }),
        linkopts = LINKOPTS_WITH_ASAN + select({
            "//:linux_x86_64": ["-fopenmp"],
            "//conditions:default": [],
        }),
        deps = [
            "@google_benchmark//:benchmark_main",
        ],
    )
    for f in glob(["*.cpp"])
]
