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
# Created: 2026/08/17

load("@rules_shell//shell:sh_binary.bzl", "sh_binary")

package(default_visibility = ["//visibility:public"])

# 生成自包含发布包 dist/minisearch.tar.gz：
#   bazel run //cpp/pl/minisearch/packaging:package
# 产物布局：bin/（minisearch + embedding 及 python 运行时）、conf/、control、
# data/、log/、run/。解压后 ./control start 一键启动。
sh_binary(
    name = "package",
    srcs = ["package.sh"],
    data = [
        "control",
        "minisearch.ini",
        "//cpp/pl/minisearch:minisearch_server",
        "//cpp/pl/minisearch/embedding_server",
    ],
    target_compatible_with = [
        "@platforms//os:macos",
        "@platforms//cpu:aarch64",
    ],
)
