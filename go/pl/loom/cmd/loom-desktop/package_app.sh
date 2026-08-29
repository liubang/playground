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
# and codesign it (docs/DESKTOP_DESIGN.md §6.2/§6.3). Identity resolution,
# in order:
#   1. $LOOM_SIGN_IDENTITY, if set (explicit override)
#   2. auto-discover "$LOOM_CERT_CN" (default "Loom Dev (liubang)") in the
#      login keychain
#   3. auto-create it by running make-signing-cert (idempotent; prompts for
#      the keychain password the first time), then use it
# A stable identity keeps firewall/TCC grants across rebuilds — ad-hoc
# signatures change the CDHash on every build and reset those grants. It
# is NOT a distribution signature (Developer ID + notarization are out of
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

# --- Signing identity resolution (see header comment) ---
CN="${LOOM_CERT_CN:-Loom Dev (liubang)}"
IDENTITY="${LOOM_SIGN_IDENTITY:-}"

# True when a codesigning identity with the default CN exists in the
# login keychain. Safe under `set -e` because callers always invoke it
# as an `if` condition.
has_identity() {
    security find-identity -v -p codesigning | grep -qF "\"$CN\""
}

if [ -z "$IDENTITY" ] && has_identity; then
    IDENTITY="$CN"
    echo "==> auto-discovered identity: $IDENTITY"
fi

if [ -z "$IDENTITY" ]; then
    # Auto-create the stable identity so the next package finds it.
    CERT_SCRIPT="$(cd "$(dirname "$0")" && pwd)/make-signing-cert"
    if [ -x "$CERT_SCRIPT" ]; then
        echo "==> no codesigning identity — running $CERT_SCRIPT"
        "$CERT_SCRIPT"
    fi
    if has_identity; then
        IDENTITY="$CN"
        echo "==> identity created: $IDENTITY"
    fi
fi

# No --deep: the bundle holds a single Mach-O, and Apple discourages
# --deep (signing order becomes unpredictable with nested code).
if [ -n "$IDENTITY" ]; then
    echo "==> signing with: $IDENTITY"
    codesign --force --sign "$IDENTITY" "${DEST}/Loom.app"
else
    echo "!! WARNING: no codesigning identity found — signing ad-hoc;"
    echo "   firewall/TCC grants will be lost on the next rebuild."
    echo "   Run once to fix: bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:make-signing-cert"
    codesign --force --sign - "${DEST}/Loom.app"
fi
# Bust Finder/LaunchServices' icon cache so a rebuilt bundle shows the
# current AppIcon.icns instead of a stale cached icon.
touch "${DEST}/Loom.app"
echo "packaged: ${DEST}/Loom.app"
