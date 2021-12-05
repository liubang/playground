#!/bin/bash

# Copyright (c) 2021 The Authors. All rights reserved.
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

# Authors: liubang (it.liubang@gmail.com)
# Created: 2021/12/05 20:25

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
