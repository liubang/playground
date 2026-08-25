#!/bin/bash
# Install the built AuraBar.app: unzip the bundle, re-sign with a stable
# local identity when AURABAR_SIGN_IDENTITY is set (keeps TCC permission
# grants across rebuilds — ad-hoc signatures change every build), and
# place it in /Applications.
#
# Usage:
#   bazel run //swift/pl/menubar:install
#   AURABAR_SIGN_IDENTITY="AuraBar Dev (liubang)" bazel run //swift/pl/menubar:install
set -euo pipefail

ZIP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
APP_NAME="AuraBar"
DEST="${AURABAR_INSTALL_DIR:-/Applications}"
IDENTITY="${AURABAR_SIGN_IDENTITY:-}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

unzip -q "$ZIP" -d "$TMP"

pkill -x "$APP_NAME" 2>/dev/null || true
rm -rf "$DEST/$APP_NAME.app"
mv "$TMP/$APP_NAME.app" "$DEST/"

if [ -n "$IDENTITY" ]; then
    echo "==> signing with: $IDENTITY"
    codesign --force --deep --sign "$IDENTITY" "$DEST/$APP_NAME.app"
fi

echo "==> installed: $DEST/$APP_NAME.app"
echo "    run with: open $DEST/$APP_NAME.app"
