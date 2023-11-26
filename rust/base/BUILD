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
# Created: 2023/11/26 22:05

#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/11/26 14:51
# Last Modified: 2023/11/26 14:51
#
#======================================================================
load("@rules_rust//rust:defs.bzl", "rust_binary")

package(default_visibility = ["//visibility:public"])

[
    rust_binary(
        name = "%s" % f[:f.rfind(".")].replace("/", "_"),
        srcs = [
            "%s" % f,
        ],
        edition = "2021",
        deps = ["@rules_rust//tools/runfiles"],
    )
    for f in glob(["src/bin/*.rs"])
]
