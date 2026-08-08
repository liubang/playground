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

# Single source of truth for the Loom version. VERSION is also read by the
# macOS packaging rules (//go/pl/loom/cmd/loom-desktop:info_plist).

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "version",
    srcs = ["version.go"],
    embedsrcs = ["VERSION"],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/version",
    visibility = ["//visibility:public"],
)

go_test(
    name = "version_test",
    srcs = ["version_test.go"],
    embed = [":version"],
)

# Exported for build-time template rendering (Info.plist).
filegroup(
    name = "version_file",
    srcs = ["VERSION"],
    visibility = ["//visibility:public"],
)
