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
# Created: 2026/07/23

load("@rules_go//go:def.bzl", "go_library", "go_test")
go_library(
    name = "app",
    srcs = [
        "bootstrap.go",
        "channel_approver.go",
        "controller.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/app",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/agent:agent_lib",
        "//go/pl/loom/internal/artifact:artifact_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/model/openai:openai_lib",
        "//go/pl/loom/internal/permission:permission_lib",
        "//go/pl/loom/internal/process:process_lib",
        "//go/pl/loom/internal/prompt:prompt",
        "//go/pl/loom/internal/render",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
        "//go/pl/loom/internal/tool/builtin:builtin_lib",
        "//go/pl/loom/internal/tool/command:command_lib",
        "//go/pl/loom/internal/tool/edit:edit_lib",
        "//go/pl/loom/internal/tool/gittools:gittools_lib",
        "//go/pl/loom/internal/tool/lint:lint_lib",
        "//go/pl/loom/internal/tool/webfetch:webfetch_lib",
        "//go/pl/loom/internal/workspace:workspace_lib",
    ],
)

go_test(
    name = "app_test",
    srcs = [
        "channel_approver_test.go",
        "controller_test.go",
    ],
    embed = [":app"],
    deps = [
        "//go/pl/loom/internal/agent:agent_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
    ],
)
