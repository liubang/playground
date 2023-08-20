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
# Created: 2023/08/20 15:58

#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/20 01:30
# Last Modified: 2023/08/20 01:30
#
#======================================================================
load("@rules_flex//flex:flex.bzl", "flex_cc_library")
load("@rules_bison//bison:bison.bzl", "bison_cc_library")

cc_library(
    name = "calc_common",
    srcs = ["calc2.c"],
    hdrs = ["calc2.h"],
    includes = ["."],
)

bison_cc_library(
    name = "calc2_bison",
    src = "calc2.y",
    deps = [":calc_common"],
)

flex_cc_library(
    name = "calc2_flex",
    src = "calc2.l",
    deps = [":calc2_bison"],
)

cc_binary(
    name = "calc2",
    srcs = [
        "main.c",
    ],
    deps = [
        ":calc2_bison",
        ":calc2_flex",
        ":calc_common",
    ],
)
