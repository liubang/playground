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

# update_homebrew_tap.sh — bump liubang/homebrew-loom to a new release.
#
# Usage:
#   update_homebrew_tap.sh <version> <dist-dir> [<tap-repo>]
#
# <version>   the release version (e.g. 20260815.1fc8010d)
# <dist-dir>  directory containing the freshly built artifacts
#             (loom-darwin-*.tar.gz, Loom-*-macos-*.dmg)
# <tap-repo>  default: https://x-access-token:${GH_TOKEN}@github.com/liubang/homebrew-loom.git
#
# Rewrites Formula/loom-cli.rb (version + URL version segment + 4 sha256)
# and Cask/loom-agent.rb (version + arm/intel sha256), then commits and
# pushes to main. Requires GH_TOKEN (PAT with contents:write on the tap).
set -euo pipefail

VERSION="${1:?usage: update_homebrew_tap.sh <version> <dist-dir> [<tap-repo>]}"
DIST="${2:?usage: update_homebrew_tap.sh <version> <dist-dir> [<tap-repo>]}"
REPO="${3:-https://x-access-token:${GH_TOKEN}@github.com/liubang/homebrew-loom.git}"

for f in \
    "${DIST}/loom-darwin-arm64.tar.gz" \
    "${DIST}/loom-darwin-amd64.tar.gz" \
    "${DIST}/loom-linux-arm64.tar.gz" \
    "${DIST}/loom-linux-amd64.tar.gz" \
    "${DIST}"/Loom-*-macos-arm64.dmg \
    "${DIST}"/Loom-*-macos-x86_64.dmg; do
    test -s "$f" || { echo "update_homebrew_tap: missing artifact $f" >&2; exit 1; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
git clone --depth 1 "$REPO" "$TMP/tap"
cd "$TMP/tap"

DARWIN_ARM="$(shasum -a 256 "${DIST}/loom-darwin-arm64.tar.gz" | awk '{print $1}')"
DARWIN_AMD="$(shasum -a 256 "${DIST}/loom-darwin-amd64.tar.gz" | awk '{print $1}')"
LINUX_ARM="$(shasum -a 256 "${DIST}/loom-linux-arm64.tar.gz" | awk '{print $1}')"
LINUX_AMD="$(shasum -a 256 "${DIST}/loom-linux-amd64.tar.gz" | awk '{print $1}')"
DMG_ARM="$(shasum -a 256 "${DIST}"/Loom-*-macos-arm64.dmg | awk '{print $1}')"
DMG_AMD="$(shasum -a 256 "${DIST}"/Loom-*-macos-x86_64.dmg | awk '{print $1}')"

python3 - "$VERSION" "$DARWIN_ARM" "$DARWIN_AMD" "$LINUX_ARM" "$LINUX_AMD" <<'PY'
import re, sys
version, darwin_arm, darwin_amd, linux_arm, linux_amd = sys.argv[1:]
p = "Formula/loom-cli.rb"
s = open(p).read()
s = re.sub(r'version "[^"]*"', f'version "{version}"', s, count=1)
# URLs hardcode the version path segment; bump them too
s = re.sub(r'(https://github\.com/liubang/playground/releases/download/)[^/"]+',
           rf'\g<1>{version}', s)
urls = {
    "loom-darwin-arm64.tar.gz": darwin_arm,
    "loom-darwin-amd64.tar.gz": darwin_amd,
    "loom-linux-arm64.tar.gz": linux_arm,
    "loom-linux-amd64.tar.gz": linux_amd,
}
def repl(m):
    url = m.group(1)
    name = url.rsplit("/", 1)[-1]
    return f'url "{url}"\n      sha256 "{urls[name]}"'
s = re.sub(r'url "([^"]*loom-[^"]*\.tar\.gz)"\n\s*sha256 "[^"]*"', repl, s)
open(p, "w").write(s)
PY

python3 - "$VERSION" "$DMG_ARM" "$DMG_AMD" <<'PY'
import re, sys
version, dmg_arm, dmg_amd = sys.argv[1:]
p = "Cask/loom-agent.rb"
s = open(p).read()
s = re.sub(r'version "[^"]*"', f'version "{version}"', s, count=1)
s = re.sub(r'sha256 arm:\s*"[^"]*"', f'sha256 arm:   "{dmg_arm}"', s, count=1)
s = re.sub(r'intel:\s*"[^"]*"', f'intel: "{dmg_amd}"', s, count=1)
open(p, "w").write(s)
PY

test -f Formula/loom-cli.rb || { echo "Formula/loom-cli.rb not found in tap" >&2; exit 1; }

git config user.name "liubang"
git config user.email "it.liubang@gmail.com"
# tap commits are bot-style; never inherit a machine-global signing config
git config commit.gpgsign false
git add -A
git commit -m "Bump loom to ${VERSION}"
git push origin HEAD:main
echo "update_homebrew_tap: pushed loom ${VERSION} to ${REPO}"
