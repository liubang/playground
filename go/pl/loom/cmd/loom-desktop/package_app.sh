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

# package_app.sh — unpack the Bazel-produced bundle zip into dist/Loom.app
# and ad-hoc codesign it (docs/DESKTOP_DESIGN.md §6.2/§6.3). Ad-hoc signing
# gives the firewall/TCC prompts a stable identity across rebuilds; it is
# NOT a distribution signature (Developer ID + notarization are out of
# scope for the desktop milestone).
set -euo pipefail

# Under `bazel run` the working directory is the runfiles root; the main
# repo appears as _main on Bazel 7+, and as the plain path otherwise. The
# bazel-bin fallback keeps the script usable when invoked directly from the
# workspace root.
ZIP_REL="go/pl/loom/cmd/loom-desktop/loom_desktop_app.zip"
ZIP=""
for base in "${RUNFILES_DIR:-$PWD}/_main" "${RUNFILES_DIR:-$PWD}" "${BUILD_WORKSPACE_DIRECTORY:-$PWD}/bazel-bin"; do
    if [[ -f "${base}/${ZIP_REL}" ]]; then
        ZIP="${base}/${ZIP_REL}"
        break
    fi
done
if [[ -z "${ZIP}" ]]; then
    echo "package_app: cannot locate loom_desktop_app.zip (build :loom_desktop_app first)" >&2
    exit 1
fi

DEST="${BUILD_WORKSPACE_DIRECTORY:-$PWD}/dist"
mkdir -p "${DEST}"
# dist holds only the LATEST packaging output: stale artifacts from a
# previous version (old Loom.app, dated DMGs, versioned debs) are
# removed before anything new lands. rm -rf on a non-matching glob is
# a no-op.
rm -rf "${DEST}/Loom.app" "${DEST}"/Loom-*.dmg "${DEST}"/loom_*.deb

unzip -q "${ZIP}" -d "${DEST}"
chmod +x "${DEST}/Loom.app/Contents/MacOS/loom-desktop"

# No --deep: the bundle holds a single Mach-O, and Apple discourages
# --deep (signing order becomes unpredictable with nested code).
codesign --force --sign - "${DEST}/Loom.app"
# Bust Finder/LaunchServices' icon cache so a rebuilt bundle shows the
# current AppIcon.icns instead of a stale cached icon.
touch "${DEST}/Loom.app"
echo "packaged: ${DEST}/Loom.app"
