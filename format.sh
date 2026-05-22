#!/usr/bin/env bash

# Copyright (c) 2023 The Authors. All rights reserved.
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
# Created: 2023/09/22 00:45

set -euo pipefail

CLANG_FORMAT='clang-format'

dirs=('cpp/features' 'cpp/meta' 'cpp/pl')

for dir in "${dirs[@]}"; do
    find "${dir}" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \) | while read -r file; do
        echo "formatting: ${file}"
        "${CLANG_FORMAT}" -i "${file}"
    done
done
