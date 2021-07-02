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
# Created: 2021/07/02 12:00

cc_binary(
    name = "test_murmurhash2",
    srcs = [
        "test_murmurhash2.cc",
        "murmurhash2.h",
    ],
    copts = [
        "-std=gnu++17",
    ],
)

cc_library(
    name = "hash",
    hdrs = [
        "murmurhash2.h",
    ],
    visibility = ["//visibility:public"],
)
