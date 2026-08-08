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

# Formats all Go sources under go/ with the hermetic gofumpt built by Bazel.
#
#   bazel run //:gofumpt            # format in place (gofumpt -w)
#   bazel run //:gofumpt -- --check # CI mode: fail if any file is unformatted
#   bazel run //:gofumpt -- -extra  # extra flags are forwarded to gofumpt

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

GOFUMPT_BIN="$(rlocation "$1")"
shift

# `bazel run` exports BUILD_WORKSPACE_DIRECTORY pointing at the workspace root.
cd "${BUILD_WORKSPACE_DIRECTORY:-.}"

# Collect Go sources under go/, excluding generated protobuf code.
files=()
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    while IFS= read -r f; do
        files+=("$f")
    done < <(git ls-files -- 'go/*.go' ':!:*.pb.go')
else
    while IFS= read -r f; do
        files+=("$f")
    done < <(find go -type f -name '*.go' ! -name '*.pb.go')
fi

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no go files found" >&2
    exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
    out="$("${GOFUMPT_BIN}" -l "${files[@]}")"
    if [[ -n "${out}" ]]; then
        echo "the following files are not gofumpt-formatted:" >&2
        echo "${out}" >&2
        exit 1
    fi
    echo "all go files are gofumpt-clean"
    exit 0
fi

exec "${GOFUMPT_BIN}" -w "$@" "${files[@]}"
