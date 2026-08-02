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

# Installs the bazel-built loom binary into GOBIN (default: $(go env GOPATH)/bin).
# Usage: bazel run //go/pl/loom/cmd/loom:install

set -euo pipefail

# The go_binary output sits next to this script inside the runfiles tree.
LOOM_BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/loom_/loom"

if [[ ! -x "${LOOM_BIN}" ]]; then
    echo "error: loom binary not found at ${LOOM_BIN}" >&2
    exit 1
fi

# Resolve target directory: $GOBIN > `go env GOBIN` > `go env GOPATH`/bin > ~/go/bin
GOBIN_DIR="${GOBIN:-}"
if [[ -z "${GOBIN_DIR}" ]] && command -v go >/dev/null 2>&1; then
    GOBIN_DIR="$(go env GOBIN)"
    if [[ -z "${GOBIN_DIR}" ]]; then
        GOBIN_DIR="$(go env GOPATH)/bin"
    fi
fi
GOBIN_DIR="${GOBIN_DIR:-${HOME}/go/bin}"

mkdir -p "${GOBIN_DIR}"
cp -f "${LOOM_BIN}" "${GOBIN_DIR}/loom"
chmod +x "${GOBIN_DIR}/loom"
echo "installed loom -> ${GOBIN_DIR}/loom"
