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

load("@rules_java//java:defs.bzl", "java_binary", "java_library")
load("@contrib_rules_jvm//java:defs.bzl", "java_junit5_test")

# Spring Boot dependencies
springboot_deps = [
    "@maven//:org_springframework_boot_spring_boot",
    "@maven//:org_springframework_boot_spring_boot_autoconfigure",
    "@maven//:org_springframework_boot_spring_boot_starter_web",
    "@maven//:org_springframework_boot_spring_boot_starter_actuator",
    "@maven//:org_springframework_spring_web",
]

# Application library
java_library(
    name = "demo_lib",
    srcs = glob(["src/main/java/**/*.java"]),
    resources = glob(["src/main/resources/**"]),
    deps = springboot_deps,
)

# Executable application
# Use `bazel run` to start, or `bazel build` to get the _deploy.jar fat jar
java_binary(
    name = "demo",
    main_class = "cc.liubang.spring.example.DemoApplication",
    runtime_deps = [":demo_lib"],
)

# Test dependencies
test_deps = [
    "@maven//:org_junit_jupiter_junit_jupiter_api",
    "@maven//:org_junit_jupiter_junit_jupiter_engine",
    "@maven//:org_junit_platform_junit_platform_launcher",
    "@maven//:org_junit_platform_junit_platform_reporting",
    "@maven//:org_springframework_boot_spring_boot_starter_test",
    "@maven//:org_springframework_boot_spring_boot_test",
    "@maven//:org_springframework_boot_spring_boot_test_autoconfigure",
    "@maven//:org_springframework_spring_beans",
    "@maven//:org_springframework_spring_test",
]

# Unit tests
java_junit5_test(
    name = "DemoApplicationTest",
    srcs = ["src/test/java/cc/liubang/spring/example/DemoApplicationTest.java"],
    test_class = "cc.liubang.spring.example.DemoApplicationTest",
    deps = [
        ":demo_lib",
    ] + test_deps,
)

java_junit5_test(
    name = "HelloControllerTest",
    srcs = ["src/test/java/cc/liubang/spring/example/HelloControllerTest.java"],
    test_class = "cc.liubang.spring.example.HelloControllerTest",
    deps = [
        ":demo_lib",
    ] + test_deps,
)
