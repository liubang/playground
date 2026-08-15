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

# package_release.sh — build the four release artifacts into dist/:
#
#   Loom-<version>-macos-arm64.dmg    desktop app, Apple silicon
#   Loom-<version>-macos-x86_64.dmg   desktop app, Intel
#   loom_<deb-version>_amd64.deb      CLI (loom chat/run/serve), Linux x86_64
#   loom_<deb-version>_arm64.deb      CLI, Linux arm64
#
# macOS bundles take their metadata + icon from the Bazel-built
# :loom_desktop_app zip; only the Mach-O is swapped per arch. All binaries
# are cross-compiled with the Go toolchain and stripped (-s -w).
#
# The Linux GUI desktop is out of scope here (needs webkit2gtk + a Linux
# build environment, docs/DESKTOP_DESIGN.md §8.4); the debs ship the CLI.
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
DEST="${WS}/dist"
mkdir -p "${DEST}"

# Version identity: "<yyyymmdd>.<git-short-hash>" from the same single
# producer the bazel builds use (tools/workspace_status.sh), injected
# into every binary below — matching the Info.plist the bazel bundle
# already carries.
LOOM_VERSION="$(bash "${WS}/tools/workspace_status.sh" | awk '$1 == "STABLE_LOOM_VERSION" {print $2}')"
[[ -n "${LOOM_VERSION}" ]] || LOOM_VERSION="dev"
VERSION_PKG="github.com/liubang/playground/go/pl/loom/internal/version"

TMP="$(mktemp -d /tmp/loom_release_XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT

# go_build <output> <goos> <goarch> <package> [extra build args...]
go_build() {
    local out="$1" goos="$2" goarch="$3" pkg="$4"
    shift 4
    (cd "${WS}/go" && GOOS="${goos}" GOARCH="${goarch}" CGO_ENABLED=0 go build \
        -ldflags="-s -w -X ${VERSION_PKG}.Version=${LOOM_VERSION}" \
        -o "${out}" "$@" "${pkg}") 1>&2
}

# --- macOS desktop (per-arch .app + DMG) ---
# The desktop app needs cgo (wails); Xcode clang accepts -arch for both
# slices with the same SDK, so plain GOARCH cross-compilation works.
# (Bazel's auto-detected cc toolchain only covers the host arch, which is
# why these builds go through the Go toolchain; the `production` tag
# mirrors --config=desktop.)
for arch in arm64 amd64; do
    [[ "${arch}" == "amd64" ]] && label="x86_64" || label="arm64"
    echo "package_release: building desktop darwin_${arch} ..." 1>&2
    (cd "${WS}/go" && GOOS=darwin GOARCH="${arch}" CGO_ENABLED=1 go build -tags production \
        -ldflags="-s -w -X ${VERSION_PKG}.Version=${LOOM_VERSION}" \
        -o "${TMP}/loom-desktop-${arch}" ./pl/loom/cmd/loom-desktop) 1>&2

    APP="${TMP}/app-${arch}/Loom.app"
    mkdir -p "${TMP}/app-${arch}"
    unzip -q "${ZIP}" -d "${TMP}/app-${arch}"
    cp "${TMP}/loom-desktop-${arch}" "${APP}/Contents/MacOS/loom-desktop"
    chmod +x "${APP}/Contents/MacOS/loom-desktop"
    codesign --force --sign - "${APP}"
    touch "${APP}"

    VERSION="$(plutil -extract CFBundleVersion raw -o - "${APP}/Contents/Info.plist")"
    DMG="${DEST}/Loom-${VERSION}-macos-${label}.dmg"
    rm -f "${DMG}"
    mkdir -p "${TMP}/dmg-${arch}"
    cp -R "${APP}" "${TMP}/dmg-${arch}/"
    ln -s /Applications "${TMP}/dmg-${arch}/Applications"
    hdiutil create -volname "Loom" -srcfolder "${TMP}/dmg-${arch}" -ov -format UDZO "${DMG}" 1>&2
    echo "packaged: ${DMG}"
done

# --- Linux CLI (.deb) ---
# Debian versions cannot contain "-" outside the revision: a stamped
# "20260815.82f4e2a" has none, but the ~ rewrite keeps hypothetical
# dash-bearing versions valid (~ sorts before any release suffix).
DEB_VERSION="${VERSION//-/~}-1"

make_deb() { # <deb-arch> <binary>
    local arch="$1" bin="$2"
    local root="${TMP}/deb-${arch}"
    mkdir -p "${root}/data/usr/bin" "${root}/control"
    cp "${bin}" "${root}/data/usr/bin/loom"
    chmod 755 "${root}/data/usr/bin/loom"
    local size_kb
    size_kb="$(du -sk "${root}/data" | cut -f1)"
    cat >"${root}/control/control" <<EOF
Package: loom
Version: ${DEB_VERSION}
Section: utils
Priority: optional
Architecture: ${arch}
Installed-Size: ${size_kb}
Maintainer: liubang <it.liubang@gmail.com>
Description: Loom - AI coding agent (CLI)
 Terminal AI coding agent: interactive chat, headless runs, and an
 HTTP/SSE server mode (loom serve) hosting the web UI.
EOF
    (cd "${root}/data" && tar -czf "${root}/data.tar.gz" .)
    (cd "${root}/control" && tar -czf "${root}/control.tar.gz" .)
    echo "2.0" >"${root}/debian-binary"
    local out="${DEST}/loom_${DEB_VERSION}_${arch}.deb"
    rm -f "${out}"
    # rcS: macOS ar auto-invokes ranlib, which discards the non-Mach-O
    # members and leaves only a __.SYMDEF entry; S skips the symbol table.
    ar rcS "${out}" "${root}/debian-binary" "${root}/control.tar.gz" "${root}/data.tar.gz"
    echo "packaged: ${out}"
}

for arch in amd64 arm64; do
    echo "package_release: building CLI linux_${arch} ..." 1>&2
    go_build "${TMP}/loom-linux-${arch}" linux "${arch}" ./pl/loom/cmd/loom
    make_deb "${arch}" "${TMP}/loom-linux-${arch}"
done
