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
# Created: 2026/08/17

# MiniSearch 发布包装配（bazel run //cpp/pl/minisearch/packaging:package）。
# 从 runfiles 取出 C++ server、python embedding server 及其完整运行时
# （embedding.runfiles 内嵌 hermetic python 解释器与全部 pip 依赖），
# 连同 control 脚本与默认配置打成 dist/minisearch.tar.gz。

set -euo pipefail

WS="${BUILD_WORKSPACE_DIRECTORY:-$(pwd)}"
cd "${WS}"
BAZEL_BIN="$(bazel info bazel-bin 2>/dev/null)"
[[ -n "${BAZEL_BIN}" ]] || {
    echo "ERROR: bazel info bazel-bin failed" >&2
    exit 1
}

SERVER="${BAZEL_BIN}/cpp/pl/minisearch/minisearch_server"
EMB="${BAZEL_BIN}/cpp/pl/minisearch/embedding_server/embedding_server"
EMB_RF="${EMB}.runfiles"
[[ -f "${SERVER}" ]] || {
    echo "ERROR: missing ${SERVER}" >&2
    exit 1
}
[[ -d "${EMB_RF}" ]] || {
    echo "ERROR: missing python runtime ${EMB_RF}" >&2
    exit 1
}
MAIN="${WS}"

DIST="${WS}/dist"
PKG="${DIST}/minisearch"
rm -rf "${PKG}"
mkdir -p "${PKG}/bin" "${PKG}/conf" "${PKG}/data" "${PKG}/log" "${PKG}/run"

cp "${SERVER}" "${PKG}/bin/minisearch"
cp "${EMB}" "${PKG}/bin/embedding"
# runfiles 树是指向 bazel-out 的符号链接，-L 解引用拷贝实体
cp -RL "${EMB_RF}" "${PKG}/bin/embedding.runfiles"

# 例外：venv 的 python3 必须保持为相对符号链接——实体拷贝会让 dyld 的
# @loader_path/../lib 解析到 venv/lib 而找不到 libpython3.13.dylib。
VENV_BIN="${PKG}/bin/embedding.runfiles/_main/cpp/pl/minisearch/embedding_server/_embedding_server.venv/bin"
PYBIN="$(ls -d "${PKG}"/bin/embedding.runfiles/rules_python++python+python_3_13_*/bin/python3 2>/dev/null | head -1)"
if [[ -n "${PYBIN}" && -d "${VENV_BIN}" ]]; then
    rm -f "${VENV_BIN}/python3"
    ln -s "$(python3 -c "import os,sys;print(os.path.relpath(sys.argv[1],sys.argv[2]))" "${PYBIN}" "${VENV_BIN}")" \
        "${VENV_BIN}/python3"
fi
cp "$(realpath "${MAIN}/cpp/pl/minisearch/packaging/control")" "${PKG}/control"
cp "$(realpath "${MAIN}/cpp/pl/minisearch/packaging/minisearch.ini")" "${PKG}/conf/minisearch.ini"
chmod +x "${PKG}/control" "${PKG}/bin/minisearch" "${PKG}/bin/embedding"

tar -C "${DIST}" -czf "${DIST}/minisearch.tar.gz" minisearch
echo "==> package: ${DIST}/minisearch.tar.gz"
du -sh "${PKG}" "${DIST}/minisearch.tar.gz"
