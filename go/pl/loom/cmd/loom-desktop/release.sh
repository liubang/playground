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

# release.sh — 一键发布桌面端：先刷新前端产物（webui → web/dist），再打包。
#
# 为什么必须拆成两步且顺序固定：JS 构建在 Bazel 依赖图外（见
# internal/server/web/BUILD 注释），package_release 的 data 依赖（.app zip）
# 在脚本运行前就已构建——把前端刷新链进 package_release.sh 内部为时已晚。
# 因此一键发布的正确形态就是这个宿主机 wrapper：串行调两条 bazel 命令。
#
# 用法：go/pl/loom/cmd/loom-desktop/release.sh

set -euo pipefail

cd "$(dirname "$0")/../../../.."

bazel run //go/pl/loom/internal/server/webui:build
exec bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:package_release
