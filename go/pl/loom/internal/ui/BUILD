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
        "blocks.go",
        "canvas.go",
        "choice.go",
        "composer.go",
        "finder.go",
        "icons.go",
        "input.go",
        "keymap.go",
        "markdown.go",
        "model.go",
        "picker.go",
        "rules_picker.go",
        "scroll.go",
        "statusbar.go",
        "subagent.go",
        "theme.go",
        "update.go",
        "view.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/ui",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/client",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/permission:permission_lib",
        "//go/pl/loom/internal/render",
        "//go/pl/loom/internal/runtimeevent",
        "//go/pl/loom/internal/session:session_lib",
        "@com_github_charmbracelet_bubbles//spinner",
        "@com_github_charmbracelet_bubbles//textarea",
        "@com_github_charmbracelet_bubbletea//:bubbletea",
        "@com_github_charmbracelet_glamour//:glamour",
        "@com_github_charmbracelet_glamour//styles",
        "@com_github_charmbracelet_lipgloss//:lipgloss",
        "@org_golang_x_sys//unix",
        "@org_golang_x_term//:term",
    ],
)

go_test(
    name = "ui_test",
    srcs = [
        "canvas_test.go",
        "choice_test.go",
        "finder_test.go",
        "follow_tail_test.go",
        "frame_stability_test.go",
        "icons_test.go",
        "input_test.go",
        "keymap_test.go",
        "markdown_test.go",
        "picker_test.go",
        "scroll_test.go",
        "subagent_test.go",
        "sync_transcript_test.go",
        "toolblocks_test.go",
        "transcript_scroll_test.go",
        "ui_test.go",
    ],
    embed = [":ui"],
    deps = [
        "//go/pl/loom/internal/agent:agent_lib",
        "//go/pl/loom/internal/app",
        "//go/pl/loom/internal/client",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/process:process_lib",
        "//go/pl/loom/internal/runtimeevent",
        "@com_github_charmbracelet_bubbletea//:bubbletea",
        "@com_github_charmbracelet_lipgloss//:lipgloss",
        "@org_golang_x_sys//unix",
    ],
)
