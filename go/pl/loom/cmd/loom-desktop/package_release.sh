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

# package_release.sh — build a universal2 (arm64 + amd64) Loom.app and wrap
# it in a distributable DMG (dist/Loom-<version>.dmg).
#
# Steps: cross-build the binary for both architectures, lipo them together,
# swap the result into the bundle produced by :loom_desktop_app (metadata +
# icon stay Bazel-built), ad-hoc sign, then pack with hdiutil.
#
# Usage: bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:package_release
set -euo pipefail

PKG="go/pl/loom/cmd/loom-desktop"
ZIP_REL="${PKG}/loom_desktop_app.zip"

# Locate the bundle zip BEFORE changing directory: under `bazel run` the
# working directory is the runfiles root (the main repo appears as _main
# on Bazel 7+), and cd-ing away would break that fallback.
ZIP=""
for base in "${RUNFILES_DIR:-$PWD}/_main" "${RUNFILES_DIR:-$PWD}" "${BUILD_WORKSPACE_DIRECTORY:-$PWD}/bazel-bin"; do
  if [[ -f "${base}/${ZIP_REL}" ]]; then
    ZIP="${base}/${ZIP_REL}"
    break
  fi
done
if [[ -z "${ZIP}" ]]; then
  echo "package_release: cannot locate ${ZIP_REL} (build :loom_desktop_app first)" >&2
  exit 1
fi

WS="${BUILD_WORKSPACE_DIRECTORY:-$PWD}"
cd "${WS}"

TMP="$(mktemp -d /tmp/loom_release_XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT

# --- universal binary ---
# cgo cross-compilation goes through the Go toolchain: Xcode clang accepts
# -arch for both slices with the same SDK, while Bazel's auto-detected cc
# toolchain only covers the host arch (the cross platform builds cgo
# packages with CGO_ENABLED=0 and fails). The `production` tag mirrors
# --config=desktop.
# -s -w strips the symbol table and DWARF (~28% smaller); release
# diagnostics rely on the file logger, so debuggability is unaffected.
# Dev builds via :package_app keep their symbols.
for arch in arm64 amd64; do
  echo "package_release: building darwin_${arch} ..." 1>&2
  (cd "${WS}/go" && GOOS=darwin GOARCH="${arch}" CGO_ENABLED=1 go build -tags production -ldflags="-s -w" -o "${TMP}/loom-desktop-${arch}" ./pl/loom/cmd/loom-desktop) 1>&2
done
lipo -create -output "${TMP}/loom-desktop" "${TMP}/loom-desktop-arm64" "${TMP}/loom-desktop-amd64"
lipo -info "${TMP}/loom-desktop" 1>&2

# --- bundle ---
DEST="${WS}/dist"
APP="${DEST}/Loom.app"
rm -rf "${APP}"
mkdir -p "${DEST}"
unzip -q "${ZIP}" -d "${DEST}"
cp "${TMP}/loom-desktop" "${APP}/Contents/MacOS/loom-desktop"
chmod +x "${APP}/Contents/MacOS/loom-desktop"
codesign --force --sign - "${APP}"
touch "${APP}"

VERSION="$(plutil -extract CFBundleVersion raw -o - "${APP}/Contents/Info.plist")"

# --- DMG ---
DMG="${DEST}/Loom-${VERSION}.dmg"
rm -f "${DMG}"
mkdir -p "${TMP}/dmg"
cp -R "${APP}" "${TMP}/dmg/"
ln -s /Applications "${TMP}/dmg/Applications"
hdiutil create -volname "Loom" -srcfolder "${TMP}/dmg" -ov -format UDZO "${DMG}" 1>&2

echo "packaged: ${APP} (universal2)"
echo "packaged: ${DMG}"
