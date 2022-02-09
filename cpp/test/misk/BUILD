# Copyright (c) 2021 The Authors. All rights reserved.
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
# Created: 2021/12/03 17:14

cc_binary(
    name = "test_call_once",
    srcs = [
        "test_call_once.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-pthread",
    ],
)

cc_test(
    name = "test_detect_class_member",
    srcs = [
        "test_detect_class_member.cpp",
        "detect_class_member.h",
    ],
    copts = [
        "-std=c++11",
    ],
    deps = [
        "@gtest//:gtest_main",
    ],
)
