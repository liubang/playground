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
# Created: 2026/09/14 20:10

load("@rules_java//java:defs.bzl", "java_binary")

# Trino parser benchmark over the same synthetic workloads as the Prism C++
# benchmark (see //cpp/pl/prism/benchmark). Results are compared in
# cpp/pl/prism/docs/benchmark.md.
java_binary(
    name = "trino_parser_benchmark",
    srcs = ["TrinoParserBenchmark.java"],
    data = ["//cpp/pl/prism/benchmark:workloads"],
    # trino-parser:468 的 class file 需要 JDK 23+（version 67），默认运行时
    # toolchain 是 JDK 21，运行时必须加 --java_runtime_version=remotejdk_25：
    #   bazel run //java/pl/prism:trino_parser_benchmark --java_runtime_version=remotejdk_25
    main_class = "pl.prism.TrinoParserBenchmark",
    deps = [
        "@bazel_tools//tools/java/runfiles",
        "@maven//:io_trino_trino_parser",
    ],
)
