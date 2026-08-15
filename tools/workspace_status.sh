#!/bin/bash
# Bazel workspace status (see .bazelrc: --workspace_status_command).
#
# Emits the STABLE keys consumed by build-time version stamping:
#
#   STABLE_LOOM_VERSION  "<yyyymmdd>.<git-short-hash>" — the Loom version
#                        string (go binaries via x_defs, Info.plist via
#                        the info_plist genrule)
#   STABLE_LOOM_DATE     "<yyyymmdd>"                  — numeric-only date
#
# STABLE keys must be identical across the two invocations Bazel makes
# per build; within a day they are (git HEAD does not change mid-build).
# Builds outside a git worktree (or without git on PATH) fall back to
# "unknown" so the build never breaks for stamping's sake.
set -u

cd "${BUILD_WORKSPACE_DIRECTORY:-$(pwd)}" || exit 0

date_part="$(date +%Y%m%d)"
hash_part="unknown"
if command -v git >/dev/null 2>&1; then
    if h="$(git rev-parse --short=8 HEAD 2>/dev/null)"; then
        hash_part="$h"
    fi
fi

echo "STABLE_LOOM_VERSION ${date_part}.${hash_part}"
echo "STABLE_LOOM_DATE ${date_part}"
