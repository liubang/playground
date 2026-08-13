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

# Authors: liubang (it.liubang@gmail.com)

# prettier 启动封装：用 multitool 锁定的 nodejs 执行 prettier.cjs，
# 并显式指定 .prettierrc，行为与机器上是否安装 node/prettier 无关。
#
# 注意：本文件是 expand_template 的模板（@NODE@/@PRETTIER@/@CONFIG@ 在
# 构建期替换为 rlocationpath）。不能用 sh_binary args 传参——rules_lint 的
# format runner 直接 exec 工具二进制，bazel run 注入的 args 不会生效。
#
# $@ = rules_lint format runner 透传的 flags + 文件列表

set -euo pipefail

# --- begin runfiles.bash initialization v3 ---
# Copy-pasted from the Bazel Bash runfiles library v3.
if [[ ! -d "${RUNFILES_DIR:-/dev/null}" && ! -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
    if [[ -f "$0.runfiles_manifest" ]]; then
        export RUNFILES_MANIFEST_FILE="$0.runfiles_manifest"
    elif [[ -f "$0.runfiles/MANIFEST" ]]; then
        export RUNFILES_MANIFEST_FILE="$0.runfiles/MANIFEST"
    elif [[ -f "$0.runfiles/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
        export RUNFILES_DIR="$0.runfiles"
    fi
fi
if [[ -f "${RUNFILES_DIR:-/dev/null}/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
    # shellcheck disable=SC1090
    source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
elif [[ -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
    # shellcheck disable=SC1090
    source "$(grep -m1 "^bazel_tools/tools/bash/runfiles/runfiles.bash " \
        "$RUNFILES_MANIFEST_FILE" | cut -d ' ' -f 2-)"
else
    echo >&2 "ERROR: cannot find @bazel_tools//tools/bash/runfiles:runfiles.bash"
    exit 1
fi
# --- end runfiles.bash initialization v3 ---

NODE_BIN="$(rlocation "@NODE@")"
PRETTIER_CJS="$(rlocation "@PRETTIER@")"
PRETTIER_RC="$(rlocation "@CONFIG@")"

exec "${NODE_BIN}" "${PRETTIER_CJS}" --config "${PRETTIER_RC}" "$@"
