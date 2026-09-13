#!/usr/bin/env bash
# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# install.sh — install the Bazel-produced Loom.app into /Applications and
# codesign it with a stable local identity (mirrors //swift/pl/aurashot:
# install). Unlike package_app.sh (whose output is dist/Loom.app for
# inspection/release), this script targets the system-level install: it
# stages the unzip next to the destination for an atomic mv, kills any
# running installed instance, and re-registers the bundle with
# LaunchServices. Identity resolution is identical to package_app.sh:
#   1. $LOOM_SIGN_IDENTITY, if set (explicit override)
#   2. auto-discover "$LOOM_CERT_CN" (default "Loom Dev (liubang)") in the
#      login keychain
#   3. auto-create it by running make-signing-cert (idempotent; prompts for
#      the keychain password the first time), then use it
# A stable identity keeps firewall/TCC grants across rebuilds; it is NOT a
# distribution signature. The installed app is NOT launched here — by repo
# rule menu-bar/app verification is done by the user:
#   open /Applications/Loom.app
set -euo pipefail

# Under `bazel run` the working directory is the runfiles root; the main
# repo appears as _main on Bazel 7+, and as the plain path otherwise. The
# bazel-bin fallback keeps the script usable when invoked directly from the
# workspace root (same resolution as package_app.sh).
ZIP_REL="go/pl/loom/cmd/loom-desktop/loom_desktop_app.zip"
ZIP=""
for base in "${RUNFILES_DIR:-$PWD}/_main" "${RUNFILES_DIR:-$PWD}" "${BUILD_WORKSPACE_DIRECTORY:-$PWD}/bazel-bin"; do
    if [[ -f "${base}/${ZIP_REL}" ]]; then
        ZIP="${base}/${ZIP_REL}"
        break
    fi
done
if [[ -z "${ZIP}" ]]; then
    echo "install: cannot locate loom_desktop_app.zip (build :loom_desktop_app first)" >&2
    exit 1
fi

DEST="${LOOM_INSTALL_DIR:-/Applications}"

# Stage the unzip next to the destination (same volume → atomic mv)
# instead of under /tmp. LaunchServices registers a .app where it first
# appears; a bundle first seen in a temp dir gets an `in-temp-dir` record
# (swift/pl/aurashot/install.sh — learned the hard way there).
STAGE="${DEST}/.Loom.staging.$$"
trap 'rm -rf "${STAGE}"' EXIT
mkdir -p "${STAGE}"

unzip -q "${ZIP}" -d "${STAGE}"
chmod +x "${STAGE}/Loom.app/Contents/MacOS/loom-desktop"

# Kill only the INSTALLED instance (-f matches the bundle's exec path), so
# a `bazel run`/`dist/` debug instance is left alone.
pkill -f "${DEST}/Loom.app/Contents/MacOS/loom-desktop" 2>/dev/null || true
rm -rf "${DEST}/Loom.app"
mv "${STAGE}/Loom.app" "${DEST}/"

# Re-register from the final path so LaunchServices' record points at the
# installed location, clearing any stale attribution.
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"
if [ -x "$LSREGISTER" ]; then
    "$LSREGISTER" -f "${DEST}/Loom.app" || true
fi

# --- Signing identity resolution (same as package_app.sh) ---
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
    # Auto-create the stable identity so the next install finds it.
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
    echo "   Run once to fix: bazel run //go/pl/loom/cmd/loom-desktop:make-signing-cert"
    codesign --force --sign - "${DEST}/Loom.app"
fi
# Bust Finder/LaunchServices' icon cache so a rebuilt bundle shows the
# current AppIcon.icns instead of a stale cached icon.
touch "${DEST}/Loom.app"

echo "==> installed: ${DEST}/Loom.app"
echo "    run with: open ${DEST}/Loom.app"
