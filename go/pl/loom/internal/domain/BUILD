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
# Created: 2026/07/22 21:10

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "domain_lib",
    srcs = [
        "context.go",
        "errors.go",
        "event.go",
        "ids.go",
        "interfaces.go",
        "lifecycle.go",
        "limits.go",
        "message.go",
        "plan.go",
        "tool.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/domain",
    visibility = ["//visibility:public"],
)

go_test(
    name = "domain_test",
    srcs = [
        "context_test.go",
        "errors_test.go",
        "event_test.go",
        "ids_test.go",
        "lifecycle_test.go",
        "limits_test.go",
        "message_test.go",
        "plan_test.go",
        "tool_test.go",
    ],
    embed = [":domain_lib"],
)
