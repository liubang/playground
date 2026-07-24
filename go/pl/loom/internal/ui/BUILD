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

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "ui",
    srcs = [
        "model.go",
        "view.go",
        "update.go",
        "blocks.go",
        "composer.go",
        "icons.go",
        "markdown.go",
        "statusbar.go",
        "theme.go",
        "picker.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/ui",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/app:app",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/render:render",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
        "@com_github_charmbracelet_bubbles//spinner",
        "@com_github_charmbracelet_bubbles//textarea",
        "@com_github_charmbracelet_bubbles//viewport",
        "@com_github_charmbracelet_bubbletea//:bubbletea",
        "@com_github_charmbracelet_glamour//:glamour",
        "@com_github_charmbracelet_glamour//styles",
        "@com_github_charmbracelet_lipgloss//:lipgloss",
    ],
)

go_test(
    name = "ui_test",
    srcs = [
        "frame_stability_test.go",
        "icons_test.go",
        "markdown_test.go",
        "toolblocks_test.go",
        "transcript_scroll_test.go",
        "ui_test.go",
    ],
    embed = [":ui"],
    deps = [
        "//go/pl/loom/internal/app:app",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
    ],
)
