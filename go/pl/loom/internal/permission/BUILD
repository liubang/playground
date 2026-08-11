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
    name = "permission_lib",
    srcs = [
        "danger.go",
        "danger_script.go",
        "decider.go",
        "domain_rules.go",
        "memory.go",
        "net_hosts.go",
        "policy.go",
        "policy_trust_other.go",
        "policy_trust_unix.go",
        "remembered_store.go",
        "rules.go",
        "tool_rules.go",
        "user_intent.go",
        "user_intent_extract.go",
        "verdict.go",
    ],
    embedsrcs = ["builtin.json"],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/permission",
    visibility = ["//visibility:public"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/process:process_lib",
        "@org_modernc_sqlite//:sqlite",
    ],
)

go_test(
    name = "permission_test",
    srcs = [
        "danger_script_test.go",
        "decider_unless_dangerous_test.go",
        "domain_rules_test.go",
        "grant_test.go",
        "memory_test.go",
        "net_hosts_test.go",
        "policy_test.go",
        "remembered_store_test.go",
        "rules_test.go",
        "tool_rules_test.go",
        "user_intent_test.go",
    ],
    embed = [":permission_lib"],
)
