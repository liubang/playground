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

load("@rules_shell//shell:sh_binary.bzl", "sh_binary")

package(default_visibility = ["//go/pl/loom:__subpackages__"])

# 前端构建桥（非 hermetic）：`bazel run //go/pl/loom/internal/server/webui:build`
# 等价于 pnpm build，产物写入 web/dist（提交入库）。JS 构建刻意留在 Bazel
# 依赖图外——web/dist 由 internal/server/web 以 embedsrcs 直接消费，改了
# 前端代码后先跑本目标再 bazel build 即可。
sh_binary(
    name = "build",
    srcs = ["build.sh"],
)
