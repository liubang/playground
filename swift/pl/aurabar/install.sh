#!/bin/bash
# Install the built AuraBar.app into /Applications, re-signing with a
# stable local identity so TCC permission grants (calendar, location…)
# survive rebuilds — ad-hoc signatures change on every build and reset
# the grants.
#
# Identity resolution, in order:
#   1. $AURABAR_SIGN_IDENTITY, if set (explicit override)
#   2. auto-discover "$AURABAR_CERT_CN" (default "AuraBar Dev (liubang)")
#      in the login keychain
#   3. auto-create it by running make-signing-cert (idempotent; prompts
#      for the keychain password the first time), then use it
# If none of these yield an identity the app is installed with its
# ad-hoc signature and a warning is printed.
#
# Usage:
#   bazel run //swift/pl/aurabar:install
#   AURABAR_SIGN_IDENTITY="AuraBar Dev (liubang)" bazel run //swift/pl/aurabar:install
set -euo pipefail

ZIP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
# Runfiles paths of the entitlement sets, passed as args 2/3 (see
# BUILD). $0-based discovery doesn't work: under `bazel run`, $0 points
# into bazel-bin, whose directory has no resources/ subtree.
ENTITLEMENTS_FULL="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
ENTITLEMENTS_DEV="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
APP_NAME="AuraBar"
DEST="${AURABAR_INSTALL_DIR:-/Applications}"
CN="${AURABAR_CERT_CN:-AuraBar Dev (liubang)}"
IDENTITY="${AURABAR_SIGN_IDENTITY:-}"

# True when a codesigning identity with the default CN exists in the
# login keychain. Safe under `set -e` because callers always invoke it
# as an `if` condition.
has_identity() {
    security find-identity -v -p codesigning | grep -qF "\"$CN\""
}

# Stage the unzip next to the destination (same volume → atomic mv)
# instead of under /tmp. LaunchServices registers a .app where it first
# appears; a bundle first seen in a temp dir gets an `in-temp-dir`
# record, which makes macOS 26's menu-bar host attribution fail to
# resolve the app on its own — its status items then get attributed to
# whatever app spawned it (e.g. the IDE whose terminal ran this
# script) and inherit that host's hidden/allowed state. That once made
# AuraBar's icons silently vanish from the menu bar.
STAGE="$DEST/.${APP_NAME}.staging.$$"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE"

unzip -q "$ZIP" -d "$STAGE"

pkill -x "$APP_NAME" 2>/dev/null || true
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
    # com.apple.weatherkit is a restricted entitlement: without a
    # provisioning profile (self-signed local identities never have
    # one) amfid refuses to spawn the app at all. Only real developer
    # identities get the full set; everything else gets the empty dev
    # set, and WeatherKit stays unavailable until then. Override with
    # AURABAR_WEATHERKIT=1/0.
    WEATHERKIT=0
    case "$IDENTITY" in
        "Developer ID Application:"*|"Apple Development:"*|"Mac Developer:"*|"Apple Distribution:"*)
            WEATHERKIT=1 ;;
    esac
    WEATHERKIT="${AURABAR_WEATHERKIT:-$WEATHERKIT}"
    if [ "$WEATHERKIT" = 1 ]; then
        ENTITLEMENTS="$ENTITLEMENTS_FULL"
    else
        ENTITLEMENTS="$ENTITLEMENTS_DEV"
    fi
    # Re-signing replaces the whole signature — pass the entitlements
    # explicitly; codesign drops the build-time set otherwise.
    SIGN_ARGS=(--force --deep --sign "$IDENTITY")
    if [ -f "$ENTITLEMENTS" ]; then
        SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
    fi
    codesign "${SIGN_ARGS[@]}" "$DEST/$APP_NAME.app"
else
    echo "!! WARNING: no codesigning identity found — AuraBar is"
    echo "   installed with an ad-hoc signature, so TCC grants"
    echo "   (calendar, location) will be lost on the next rebuild."
    echo "   Run once to fix: bazel run //swift/pl/aurabar:make-signing-cert"
fi

echo "==> installed: $DEST/$APP_NAME.app"
echo "    run with: open $DEST/$APP_NAME.app"
