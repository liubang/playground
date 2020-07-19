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

COPTS_COMMON = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Wmissing-declarations",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wno-unused-local-typedefs",
    "-Wno-unused-parameter",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
    "-Wno-sign-compare",
    "-msse3",
    "-msse4",
    "-msse4.1",
    "-mpclmul",
    "-fno-builtin-memcmp",
]

C11_OPTS = [
    "-std=gnu++11",
] + COPTS_COMMON

C14_OPTS = [
    "-std=gnu++14",
] + COPTS_COMMON

C2A_OPTS = [
    "-std=gnu++2a",
] + COPTS_COMMON

cc_binary(
    name = "unique_ptr",
    srcs = ["unique_ptr.cc"],
    copts = C14_OPTS,
)

cc_binary(
    name = "weak_ptr",
    srcs = ["weak_ptr.cc"],
    copts = C14_OPTS,
)

cc_binary(
    name = "2.1-nullptr",
    srcs = ["2.1-nullptr.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "7.1-concurrency",
    srcs = ["7.1-concurrency.cc"],
    copts = C2A_OPTS,
    linkopts = ["-pthread"],
)

cc_binary(
    name = "7.4-condition",
    srcs = ["7.4-condition.cc"],
    copts = C2A_OPTS,
    linkopts = ["-pthread"],
)

cc_binary(
    name = "7.5-atomic",
    srcs = ["7.5-atomic.cc"],
    copts = C2A_OPTS,
    linkopts = ["-pthread"],
)

cc_binary(
    name = "7.5-atomic_is_lock_free",
    srcs = ["7.5-atomic_is_lock_free.cc"],
    copts = C2A_OPTS,
    linkopts = ["-latomic"],
)

cc_binary(
    name = "4.1-container-vector",
    srcs = ["4.1-container-vector.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "4.1-container-array",
    srcs = ["4.1-container-array.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "4.3-container-tuple",
    srcs = ["4.3-container-tuple.cc"],
    copts = C2A_OPTS,
)

