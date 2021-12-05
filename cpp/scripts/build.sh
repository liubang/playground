#!/bin/bash

set -eou pipefail

COMMIT_RANGE=$(git merge-base origin/master HEAD^)".." && readonly COMMIT_RANGE

bazel_query() {
	npx bazel query --keep_going "set($1)" || true
}

main() {
	local affected && affected=$(
		git diff --name-only --diff-filter=d "$COMMIT_RANGE" | tr '\n' ' '
	)

	local files && files=$(bazel_query "$affected")

	echo "${files[*]}" | tr '\n' ' ' >affected
}

main
