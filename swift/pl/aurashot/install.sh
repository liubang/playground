#!/bin/bash
# Install the built AuraShot.app into /Applications, re-signing with a
# stable local identity so the TCC screen-recording grant survives
# rebuilds — ad-hoc signatures change on every build and reset it.
#
# Identity resolution, in order:
#   1. $AURASHOT_SIGN_IDENTITY, if set (explicit override)
#   2. auto-discover "$AURASHOT_CERT_CN" (default "AuraShot Dev (liubang)")
#      in the login keychain
#   3. auto-create it by running make-signing-cert (idempotent; prompts
#      for the keychain password the first time), then use it
# If none of these yield an identity the app is installed with its
# ad-hoc signature and a warning is printed.
#
# Usage:
#   bazel run //swift/pl/aurashot:install
#   AURASHOT_SIGN_IDENTITY="AuraShot Dev (liubang)" bazel run //swift/pl/aurashot:install
set -euo pipefail

ZIP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
# Runfiles path of the entitlements file, passed as arg 2 (see BUILD).
ENTITLEMENTS="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
APP_NAME="AuraShot"
DEST="${AURASHOT_INSTALL_DIR:-/Applications}"
CN="${AURASHOT_CERT_CN:-AuraShot Dev (liubang)}"
IDENTITY="${AURASHOT_SIGN_IDENTITY:-}"

# True when a codesigning identity with the default CN exists in the
# login keychain. Safe under `set -e` because callers always invoke it
# as an `if` condition.
has_identity() {
    security find-identity -v -p codesigning | grep -qF "\"$CN\""
}

# Stage the unzip next to the destination (same volume → atomic mv)
# instead of under /tmp. LaunchServices registers a .app where it first
# appears; a bundle first seen in a temp dir gets an `in-temp-dir`
# record, which makes the menu-bar host attribution fail to resolve the
# app on its own.
STAGE="$DEST/.${APP_NAME}.staging.$$"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE"

unzip -q "$ZIP" -d "$STAGE"

pkill -x "$APP_NAME" 2>/dev/null || true
# Kill a leftover bundled OCR server from the previous install: it was
# spawned from inside the old bundle, and a running instance would keep
# the port busy so the new app would keep talking to the old binary.
pkill -f "$APP_NAME.app/Contents/Resources/mllm_server" 2>/dev/null || true
rm -rf "$DEST/$APP_NAME.app"
mv "$STAGE/$APP_NAME.app" "$DEST/"

# Re-register from the final path so LaunchServices' record points at
# the installed location, clearing any stale in-temp-dir attribution.
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"
if [ -x "$LSREGISTER" ]; then
    "$LSREGISTER" -f "$DEST/$APP_NAME.app" || true
fi

# Resolve the signing identity (see header comment).
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

if [ -n "$IDENTITY" ]; then
    echo "==> signing with: $IDENTITY"
    # Re-signing replaces the whole signature — pass the entitlements
    # explicitly; codesign drops the build-time set otherwise.
    SIGN_ARGS=(--force --deep --sign "$IDENTITY")
    if [ -f "$ENTITLEMENTS" ]; then
        SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
    fi
    codesign "${SIGN_ARGS[@]}" "$DEST/$APP_NAME.app"
else
    echo "!! WARNING: no codesigning identity found — AuraShot is"
    echo "   installed with an ad-hoc signature, so the TCC screen-"
    echo "   recording grant will be lost on the next rebuild."
    echo "   Run once to fix: bazel run //swift/pl/aurashot:make-signing-cert"
fi

echo "==> installed: $DEST/$APP_NAME.app"
echo "    run with: open $DEST/$APP_NAME.app"
