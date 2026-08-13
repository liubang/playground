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
# Created: 2023/04/14 14:00

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

licenses(["notice"])

exports_files([
    "LICENSE",
    "MODULE.bazel",
    "WORKSPACE",
    # //tools/format:prettier 的运行时配置
    ".prettierrc",
])

config_setting(
    name = "linux",
    constraint_values = [
        "@platforms//os:linux",
    ],
)

config_setting(
    name = "linux_x86_64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "linux_arm",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:arm",
    ],
)

config_setting(
    name = "linux_aarch64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:aarch64",
    ],
)

config_setting(
    name = "darwin",
    constraint_values = [
        "@platforms//os:osx",
    ],
)

config_setting(
    name = "osx_arm64",
    constraint_values = [
        "@platforms//os:osx",
        "@platforms//cpu:aarch64",
    ],
)

config_setting(
    name = "osx_x86_64",
    constraint_values = [
        "@platforms//os:osx",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "windows_x86_64",
    constraint_values = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {
        "//cpp/...": "",
    },
)

# 统一代码格式化（C++/Go/Java/Python/JS/CSS/HTML/Shell/Starlark/YAML）：
#   bazel run //:format        # 全量格式化
#   bazel run //:format <path> # 只格式化指定文件
#   bazel run //:format.check  # CI 检查模式
alias(
    name = "format",
    actual = "//tools/format",
)

alias(
    name = "format.check",
    actual = "//tools/format:format.check",
)
