#!/usr/bin/env bash

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

# WebUI 前端构建（非 hermetic 便利封装）：
#   bazel run //go/pl/loom/internal/server/webui:build
# 等价于在 webui 目录下执行 pnpm build；产物输出到 web/dist（提交入库，
# 由 internal/server/web 经 embed.FS 内嵌）。依赖宿主机 node/pnpm，
# JS 构建刻意留在 Bazel 依赖图外（见 web/BUILD 注释），此脚本只是桥。

set -euo pipefail

if [[ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
    # bazel run 注入的工作区根
    cd "${BUILD_WORKSPACE_DIRECTORY}/go/pl/loom/internal/server/webui"
else
    cd "$(dirname "$0")"
fi

# 环境前置检查：只有改前端代码才需要 node/pnpm（bazel build 消费 dist
# 不需要）。推荐 corepack 按 package.json 的 packageManager 字段自动供应
# 锁定版本的 pnpm；mise/nvm 等亦可，node>=22（engines 字段）。
if ! command -v pnpm >/dev/null 2>&1; then
    echo >&2 "ERROR: 未找到 pnpm。前端构建需要 node>=22 与 pnpm@11.9.0："
    echo >&2 "  corepack enable   # 按 packageManager 字段自动供应（推荐）"
    echo >&2 "或参考 webui/package.json 的 engines/packageManager 字段安装。"
    exit 1
fi

if [[ ! -d node_modules ]]; then
    pnpm install --frozen-lockfile
fi

exec pnpm build
