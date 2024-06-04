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
# Created: 2023/05/22 01:02

#======================================================================
#
# repos.bzl -
#
# Created by liubang on 2023/05/21 23:54
# Last Modified: 2023/05/21 23:54
#
#======================================================================

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def external_repositories():
    http_archive(
        name = "rules_ragel",
        urls = ["https://github.com/jmillikin/rules_ragel/archive/07490ea288899d816bddadfb2ae1393d6a9b9c1c.zip"],
        sha256 = "9891b1925a0a539bd4d5ab1e0997f42fa72b50a0483b3f2bdf39861e44f16df0",
        strip_prefix = "rules_ragel-07490ea288899d816bddadfb2ae1393d6a9b9c1c",
    )
