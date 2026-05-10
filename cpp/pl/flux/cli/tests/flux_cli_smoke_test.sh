#!/bin/bash

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

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/04/19 20:08

set -euo pipefail

workspace_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
flux_bin="${workspace_root}/cpp/pl/flux/flux"
example="${workspace_root}/cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux"

list_output="$("${flux_bin}" --list-results "${example}")"
expected_list=$'latest_west_cpu\nlatest_east_mem'
if [[ "${list_output}" != "${expected_list}" ]]; then
    echo "unexpected --list-results output:"
    printf '%s\n' "${list_output}"
    exit 1
fi

filtered_output="$("${flux_bin}" --result latest_east_mem "${example}")"
if [[ "${filtered_output}" != *"Result: latest_east_mem"* ]]; then
    echo "missing filtered result header"
    printf '%s\n' "${filtered_output}"
    exit 1
fi
if [[ "${filtered_output}" == *"latest_west_cpu"* ]]; then
    echo "unexpected extra result in filtered output"
    printf '%s\n' "${filtered_output}"
    exit 1
fi
