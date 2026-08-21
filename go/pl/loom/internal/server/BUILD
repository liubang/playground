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
# Created: 2026/08/04

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "server",
    srcs = [
        "auth.go",
        "errors.go",
        "handlers_artifacts.go",
        "handlers_config.go",
        "handlers_events.go",
        "handlers_maze.go",
        "handlers_mcp.go",
        "handlers_packs.go",
        "handlers_sessions.go",
        "handlers_share.go",
        "handlers_skills.go",
        "handlers_workspaces.go",
        "lock.go",
        "server.go",
        "share_manager.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/server",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/mcp:mcp_lib",
        "//go/pl/loom/internal/permission:permission_lib",
        "//go/pl/loom/internal/runtimeevent",
        "//go/pl/loom/internal/server/web",
        "//go/pl/loom/internal/skill:skill_lib",
        "@org_golang_x_sys//unix",
    ],
)

go_test(
    name = "server_test",
    srcs = [
        "errors_test.go",
        "handlers_artifacts_test.go",
        "handlers_config_test.go",
        "handlers_export_test.go",
        "handlers_maze_test.go",
        "handlers_packs_test.go",
        "handlers_share_test.go",
        "handlers_skills_test.go",
        "middleware_test.go",
        "server_test.go",
        "share_manager_test.go",
    ],
    embed = [":server"],
    deps = [
        "//go/pl/loom/internal/agent:agent_lib",
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/artifact:artifact_lib",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
        "//go/pl/loom/internal/trace",
    ],
)
