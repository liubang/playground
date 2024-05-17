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

load("@bazel_skylib//lib:selects.bzl", "selects")
load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

licenses(["notice"])

exports_files([
    "LICENSE",
    "MODULE.bazel",
    "WORKSPACE",
])

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

selects.config_setting_group(
    name = "brpc_with_glog",
    match_any = [
        ":brpc_with_glog_deprecated_flag",
        ":brpc_with_glog_new_flag",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "brpc_with_glog_deprecated_flag",
    define_values = {"with_glog": "true"},
)

config_setting(
    name = "brpc_with_glog_new_flag",
    define_values = {"BRPC_WITH_GLOG": "true"},
)

selects.config_setting_group(
    name = "brpc_with_mesalink",
    match_any = [
        ":brpc_with_mesalink_deprecated_flag",
        ":brpc_with_mesalink_new_flag",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "brpc_with_mesalink_deprecated_flag",
    define_values = {"with_mesalink": "true"},
)

config_setting(
    name = "brpc_with_mesalink_new_flag",
    define_values = {"BRPC_WITH_MESALINK": "true"},
)

selects.config_setting_group(
    name = "brpc_with_thrift",
    match_any = [
        ":brpc_with_thrift_deprecated_flag",
        ":brpc_with_thrift_new_flag",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "brpc_with_thrift_legacy_version",
    define_values = {"BRPC_WITH_THRIFT_LEGACY_VERSION": "true"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "brpc_with_thrift_deprecated_flag",
    define_values = {"with_thrift": "true"},
)

config_setting(
    name = "brpc_with_thrift_new_flag",
    define_values = {"BRPC_WITH_THRIFT": "true"},
)

config_setting(
    name = "brpc_build_for_unittest",
    define_values = {"BRPC_BUILD_FOR_UNITTEST": "true"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "brpc_with_sse42",
    define_values = {"BRPC_WITH_SSE42": "true"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "darwin",
    values = {"cpu": "darwin"},
    visibility = ["//:__subpkgs__"],
)

config_setting(
    name = "brpc_with_rdma",
    define_values = {"BRPC_WITH_RDMA": "true"},
    visibility = ["//visibility:public"],
)

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {
        "//cpp/...": "",
    },
)
