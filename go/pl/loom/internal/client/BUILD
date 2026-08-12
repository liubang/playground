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
# Created: 2026/08/03

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "client",
    srcs = [
        "client.go",
        "http.go",
        "inproc.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/client",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/model/sse:sse_lib",
        "//go/pl/loom/internal/permission:permission_lib",
        "//go/pl/loom/internal/runtimeevent",
    ],
)

go_test(
    name = "client_test",
    srcs = [
        "client_test.go",
        "contract_test.go",
    ],
    embed = [":client"],
    deps = [
        "//go/pl/loom/internal/agent:agent_lib",
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/permission:permission_lib",
        "//go/pl/loom/internal/runtimeevent",
        "//go/pl/loom/internal/server",
        "//go/pl/loom/internal/session:session_lib",
    ],
)
