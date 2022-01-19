# Copyright (c) 2022 The Authors. All rights reserved.
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
# Created: 2022/01/17 00:02

cc_test(
    name = "lexer_test",
    srcs = [
        "lexer.cpp",
        "lexer.h",
        "ut/lexer_test.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "@gtest//:gtest_main",
    ],
)

cc_binary(
    name = "bfcc",
    srcs = [
        "bfcc.cpp",
        "lexer.h",
        "lexer.cpp",
    ],
    copts = [
        "-std=c++17",
        "-fsanitize=address",
        "-fno-omit-frame-pointer",
    ],
)
