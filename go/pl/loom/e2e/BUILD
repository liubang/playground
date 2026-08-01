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
# Created: 2026/07/26

load("@rules_go//go:def.bzl", "go_test")

go_test(
    name = "e2e_test",
    srcs = [
        "context_budget_e2e_test.go",
        "exsession_e2e_test.go",
        "mcp_e2e_test.go",
        "permission_e2e_test.go",
        "skills_e2e_test.go",
        "subagent_e2e_test.go",
    ],
    deps = [
        "//go/pl/loom/internal/agent:agent_lib",
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/artifact:artifact_lib",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/model/openai:openai_lib",
        "//go/pl/loom/internal/permission:permission_lib",
        "//go/pl/loom/internal/process:process_lib",
        "//go/pl/loom/internal/prompt:prompt",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
        "//go/pl/loom/internal/skill:skill_lib",
        "//go/pl/loom/internal/tool/builtin:builtin_lib",
        "//go/pl/loom/internal/tool/command:command_lib",
        "//go/pl/loom/internal/tool/edit:edit_lib",
        "//go/pl/loom/internal/tool/exsession:exsession_lib",
        "//go/pl/loom/internal/tool/subagent:subagent_lib",
        "//go/pl/loom/internal/workspace:workspace_lib",
        "@in_gopkg_yaml_v3//:yaml_v3",
    ],
)
