#!/bin/bash
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
