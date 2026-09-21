#!/bin/bash
# Install the built Loom Native.app into /Applications, re-signing with a
# stable local identity (ad-hoc signatures change on every build).
#
# Identity resolution, in order:
#   1. $LOOMNATIVE_SIGN_IDENTITY, if set (explicit override)
#   2. auto-discover "$LOOMNATIVE_CERT_CN" (default "LoomNative Dev (liubang)")
#      in the login keychain
#   3. auto-create it by running make-signing-cert (idempotent; prompts
#      for the keychain password the first time), then use it
# If none of these yield an identity the app keeps its ad-hoc signature.
#
# Usage:
#   bazel run //swift/pl/loom:install
set -euo pipefail

ZIP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
APP_NAME="Loom"
DEST="${LOOMNATIVE_INSTALL_DIR:-/Applications}"
CN="${LOOMNATIVE_CERT_CN:-LoomNative Dev (liubang)}"
IDENTITY="${LOOMNATIVE_SIGN_IDENTITY:-}"

has_identity() {
    security find-identity -v -p codesigning | grep -qF "\"$CN\""
}

# Stage the unzip next to the destination (same volume → atomic mv) so
# LaunchServices' first sight of the bundle is its final path.
STAGE="$DEST/.${APP_NAME}.staging.$$"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE"

unzip -q "$ZIP" -d "$STAGE"
# The bundled CLI must be executable regardless of zip permission bits.
chmod +x "$STAGE/$APP_NAME.app/Contents/Resources/loom" 2>/dev/null || true

pkill -x "$APP_NAME" 2>/dev/null || true
rm -rf "$DEST/$APP_NAME.app"
mv "$STAGE/$APP_NAME.app" "$DEST/"
# Bazel zips carry normalized (1980) mtimes; without a fresh bundle
# mtime LaunchServices never invalidates its icon cache and Finder/Dock
# keep showing the previous install's icon.
touch "$DEST/$APP_NAME.app"

LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"
if [ -x "$LSREGISTER" ]; then
    "$LSREGISTER" -f "$DEST/$APP_NAME.app" || true
fi

if [ -z "$IDENTITY" ] && has_identity; then
    IDENTITY="$CN"
    echo "==> auto-discovered identity: $IDENTITY"
fi

if [ -z "$IDENTITY" ]; then
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

if [ -n "$IDENTITY" ]; then
    echo "==> signing with: $IDENTITY"
    # Sign the embedded CLI first (inside-out), then the whole bundle.
    if [ -f "$DEST/$APP_NAME.app/Contents/Resources/loom" ]; then
        codesign --force --sign "$IDENTITY" "$DEST/$APP_NAME.app/Contents/Resources/loom"
    fi
    codesign --force --deep --sign "$IDENTITY" "$DEST/$APP_NAME.app"
else
    echo "!! WARNING: no codesigning identity found — installed with an"
    echo "   ad-hoc signature. Run once to fix:"
    echo "   bazel run //swift/pl/loom:make-signing-cert"
fi

echo "==> installed: $DEST/$APP_NAME.app"
echo "    run with: open $DEST/$APP_NAME.app"
