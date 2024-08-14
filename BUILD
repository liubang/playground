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

load("@gazelle//:def.bzl", "gazelle", "gazelle_binary")
load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

licenses(["notice"])

# gazelle:java_maven_install_file maven_install.json
# gazelle:exclude cpp
# gazelle:exclude proto
# gazelle:exclude **/target
gazelle(
    name = "gazelle",
    args = ["-r"],
    gazelle = ":gazelle_bin",
)

gazelle_binary(
    name = "gazelle_bin",
    languages = [
        "@gazelle//language/proto:go_default_library",
        "@contrib_rules_jvm//java/gazelle",
    ],
    visibility = ["//visibility:private"],
)

exports_files([
    "LICENSE",
    "MODULE.bazel",
    "WORKSPACE",
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
