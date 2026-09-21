#!/bin/bash
# Create a self-signed code-signing certificate for Loom Native and
# import it into the login keychain. A stable identity keeps the
# designated requirement constant across rebuilds (ad-hoc signatures
# change every build). Good enough for personal machines; distribution
# still needs an Apple Developer account.
#
# Idempotent: if the identity already exists in the keychain, do nothing.
#
# Usage:
#   bazel run //swift/pl/loom:make-signing-cert
#   LOOMNATIVE_CERT_CN="My Cert Name" bazel run //swift/pl/loom:make-signing-cert
set -euo pipefail

CN="${LOOMNATIVE_CERT_CN:-LoomNative Dev (liubang)}"
DIR="${LOOMNATIVE_CERT_DIR:-$HOME/.config/loom-native/codesign}"
DAYS="${LOOMNATIVE_CERT_DAYS:-3650}"

echo "==> identity: $CN"

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

# macOS' security(1) only accepts legacy/SHA1 PKCS#12 algorithms.
PASS="$(openssl rand -hex 12)"
if ! openssl pkcs12 -export -legacy \
    -out loomnative-dev.p12 -inkey key.pem -in cert.pem -passout "pass:$PASS" 2>/dev/null; then
    openssl pkcs12 -export \
        -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 \
        -out loomnative-dev.p12 -inkey key.pem -in cert.pem -passout "pass:$PASS"
fi

echo "==> importing into login keychain"
security import loomnative-dev.p12 \
    -k "$HOME/Library/Keychains/login.keychain-db" \
    -P "$PASS" -T /usr/bin/codesign

echo "==> trusting certificate for code signing"
security add-trusted-cert -r trustRoot \
    -k "$HOME/Library/Keychains/login.keychain-db" cert.pem

echo "==> verifying"
security find-identity -v -p codesigning | grep -F "\"$CN\""

echo
echo "Done. Next: bazel run //swift/pl/loom:install"
echo "Certificate material kept in $DIR (p12 password: $PASS)."
