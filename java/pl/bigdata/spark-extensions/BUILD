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

java_library(
    name = "spark_extensions",
    srcs = glob(["src/main/java/**/*.java"]),
    javacopts = [
        "--release",
        "17",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@maven//:org_apache_hadoop_hadoop_client_api",
        "@maven//:org_apache_hive_hive_exec_core",
        "@maven//:org_apache_spark_spark_sql_2_13",
    ],
)

java_junit5_test(
    name = "ReverseUdfTest",
    srcs = ["src/test/java/io/github/liubang/spark/ReverseUdfTest.java"],
    javacopts = [
        "--release",
        "17",
    ],
    test_class = "io.github.liubang.spark.ReverseUdfTest",
    deps = [
        ":spark_extensions",
        "@maven//:org_apache_hadoop_hadoop_client_api",
        "@maven//:org_junit_jupiter_junit_jupiter_api",
        "@maven//:org_junit_jupiter_junit_jupiter_engine",
        "@maven//:org_junit_platform_junit_platform_launcher",
        "@maven//:org_junit_platform_junit_platform_reporting",
    ],
)

genrule(
    name = "spark_extension_jar",
    srcs = [":spark_extensions"],
    outs = ["spark-extensions.jar"],
    cmd = "cp $(location :spark_extensions) $@",
    visibility = ["//visibility:public"],
)
