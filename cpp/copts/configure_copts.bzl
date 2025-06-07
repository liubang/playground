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

load(
    "//cpp:copts/copts.bzl",
    "ASAN_FLAGS",
    "GCC_FLAGS",
    "GCC_TEST_FLAGS",
    "LLVM_FLAGS",
    "LLVM_TEST_FLAGS",
)

COPTS = select({
    "//cpp:clang_compiler": LLVM_FLAGS,
    "//cpp:gcc_compiler": GCC_FLAGS,
    "//conditions:default": GCC_FLAGS,
})

DEFAULT_COPTS = select({
    "//cpp:clang_compiler": LLVM_FLAGS,
    "//cpp:gcc_compiler": GCC_FLAGS + ASAN_FLAGS,
    "//conditions:default": GCC_FLAGS + ASAN_FLAGS,
})

TEST_COPTS = select({
    "//cpp:clang_compiler": LLVM_TEST_FLAGS,
    "//cpp:gcc_compiler": GCC_TEST_FLAGS,
    "//conditions:default": GCC_TEST_FLAGS,
})

DEFAULT_LINKOPTS = select({
    "//cpp:clang_compiler": [],
    "//conditions:default": [
        "-fsanitize=address",
    ],
})
