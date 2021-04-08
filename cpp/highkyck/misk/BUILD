# Copyright (c) 2020 The Authors. All rights reserved.
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
# Created: 2020/07/20 00:32

[
    cc_binary(
        name = "%s" % f[0:f.rfind(".")],
        srcs = [
            "%s" % f,
        ] + glob([
            "includes/*.h",
        ]),
        includes = [
            "includes",
        ],
        copts = [
            "-std=gnu++17",
            "-w",
        ],
    )
    for f in glob(["test_*.cc"])
]

[
    cc_test(
        name = "%s" % f[0:f.rfind(".")],
        srcs = [
            "%s" % f,
        ] + glob([
            "includes/*.h",
        ]),
        includes = [
            "includes",
        ],
        linkopts = [
            "-pthread",
        ],
        copts = [
            "-std=gnu++17",
            "-w",
        ],
        deps = [
            "@gtest//:gtest_main",
        ],
    )
    for f in glob(["*_test.cc"])
]
