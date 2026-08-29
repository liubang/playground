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

# Create a self-signed code-signing certificate for Loom and import it
# into the login keychain.
#
# Why: ad-hoc signatures change on every rebuild, so firewall/TCC
# permission grants (incoming connections, Desktop/Documents access) are
# lost each time. A stable self-signed identity keeps the designated
# requirement constant across rebuilds — grant once, keep forever. Good
# enough for personal machines; distribution still needs an Apple
# Developer ID + notarization (see package_release.sh).
#
# Idempotent: if the identity already exists in the keychain, do nothing.
#
# Usage:
#   bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:make-signing-cert
#   LOOM_CERT_CN="My Cert Name" bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:make-signing-cert
set -euo pipefail

CN="${LOOM_CERT_CN:-Loom Dev (liubang)}"
DIR="${LOOM_CERT_DIR:-$HOME/.config/loom/codesign}"
DAYS="${LOOM_CERT_DAYS:-3650}"

echo "==> identity: $CN"

# Idempotency: an existing codesigning identity with this CN wins.
if security find-identity -v -p codesigning | grep -qF "\"$CN\""; then
    echo "==> already present in keychain, nothing to do"
    security find-identity -v -p codesigning | grep -F "\"$CN\"" || true
    exit 0
fi

mkdir -p "$DIR"
cd "$DIR"

cat >openssl.cnf <<EOF
[ req ]
distinguished_name = dn
x509_extensions    = ext
prompt             = no

[ dn ]
CN = $CN

[ ext ]
basicConstraints     = critical, CA:true
keyUsage             = critical, digitalSignature
extendedKeyUsage     = critical, codeSigning
subjectKeyIdentifier = hash
EOF

echo "==> generating key and self-signed certificate ($DAYS days)"
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout key.pem -out cert.pem \
    -days "$DAYS" -config openssl.cnf 2>/dev/null

# macOS' security(1) only accepts legacy/SHA1 PKCS#12 algorithms; OpenSSL 3
# defaults to SHA-256 which fails with "MAC verification failed".
PASS="$(openssl rand -hex 12)"
if ! openssl pkcs12 -export -legacy \
    -out loom-dev.p12 -inkey key.pem -in cert.pem -passout "pass:$PASS" 2>/dev/null; then
    openssl pkcs12 -export \
        -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 \
        -out loom-dev.p12 -inkey key.pem -in cert.pem -passout "pass:$PASS"
fi

echo "==> importing into login keychain"
security import loom-dev.p12 \
    -k "$HOME/Library/Keychains/login.keychain-db" \
    -P "$PASS" -T /usr/bin/codesign

echo "==> trusting certificate for code signing"
security add-trusted-cert -r trustRoot \
    -k "$HOME/Library/Keychains/login.keychain-db" cert.pem

echo "==> verifying"
security find-identity -v -p codesigning | grep -F "\"$CN\""

cat <<EOF

Done. Next steps:

  bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:package_app

Certificate material kept in $DIR (key.pem / cert.pem are the source of
truth; loom-dev.p12 password: $PASS — only needed to re-import the p12).
EOF
