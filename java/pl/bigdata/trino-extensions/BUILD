# Copyright (c) 2026 The Authors. All rights reserved.
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
# Created: 2026/07/15 13:28

load("@contrib_rules_jvm//java:defs.bzl", "java_junit5_test")
load("@rules_java//java:defs.bzl", "java_library")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files", "strip_prefix")
load("@rules_pkg//pkg:zip.bzl", "pkg_zip")

java_library(
    name = "trino_extensions",
    srcs = glob(["src/main/java/**/*.java"]),
    resource_strip_prefix = "java/pl/bigdata/trino-extensions/src/main/resources",
    resources = ["src/main/resources/META-INF/services/io.trino.spi.Plugin"],
    visibility = ["//visibility:public"],
    deps = [
        "@maven//:io_airlift_slice",
        "@maven//:io_trino_trino_spi",
    ],
)

java_junit5_test(
    name = "E2eFunctionsTest",
    srcs = ["src/test/java/io/github/liubang/trino/E2eFunctionsTest.java"],
    test_class = "io.github.liubang.trino.E2eFunctionsTest",
    deps = [
        ":trino_extensions",
        "@maven//:io_airlift_slice",
        "@maven//:org_junit_jupiter_junit_jupiter_api",
        "@maven//:org_junit_jupiter_junit_jupiter_engine",
        "@maven//:org_junit_platform_junit_platform_launcher",
        "@maven//:org_junit_platform_junit_platform_reporting",
    ],
)

pkg_files(
    name = "trino_plugin_files",
    srcs = [":trino_extensions"],
    prefix = "e2e-functions",
    renames = {
        ":trino_extensions": "trino-extensions.jar",
    },
    strip_prefix = strip_prefix.files_only(),
)

pkg_zip(
    name = "trino_plugin_zip",
    srcs = [":trino_plugin_files"],
    out = "e2e-functions.zip",
    mode = "0644",
    visibility = ["//visibility:public"],
)
