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

genrule(
    name = "calc_lemon",
    srcs = [
        "calc.y",
        "ttokens.h",
        "@lemon//:lempar",
    ],
    outs = [
        "calc.c",
    ],
    cmd = "$(location @lemon//:lemon) -T$(location @lemon//:lempar) -d$(RULEDIR) -s $(location calc.y)",
    tools = [
        "@lemon",
    ],
)

cc_binary(
    name = "calc",
    srcs = [
        "main.cpp",
        "ttokens.h",
        "types.h",
        ":calc_lemon",
    ],
    includes = ["."],
)
