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
# Created: 2026/04/25 11:09

set -euo pipefail

workspace_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
flux_bin="${workspace_root}/cpp/pl/flux/cli/flux"
examples_dir="${workspace_root}/cpp/pl/flux/examples/stdlib_conformance"
checked_names=()

remember() {
    checked_names+=("$1")
}

check() {
    local name="$1"
    local expected="$2"
    local output
    remember "${name}"
    output="$("${flux_bin}" --output-format json --result _result "${examples_dir}/${name}.flux")"
    if [[ "${output}" != "${expected}" ]]; then
        echo "unexpected stdlib conformance output for ${name}:"
        printf 'expected: %s\n' "${expected}"
        printf 'actual:   %s\n' "${output}"
        exit 1
    fi
}

check_regex() {
    local name="$1"
    local pattern="$2"
    local output
    remember "${name}"
    output="$("${flux_bin}" --output-format json --result _result "${examples_dir}/${name}.flux")"
    if [[ ! "${output}" =~ ${pattern} ]]; then
        echo "unexpected stdlib conformance output for ${name}:"
        printf 'pattern: %s\n' "${pattern}"
        printf 'actual:  %s\n' "${output}"
        exit 1
    fi
}

assert_registry_complete() {
    local file
    local name
    local checked
    local found

    for file in "${examples_dir}"/*.flux; do
        name="$(basename "${file}" .flux)"
        found=false
        for checked in "${checked_names[@]}"; do
            if [[ "${checked}" == "${name}" ]]; then
                found=true
                break
            fi
        done
        if [[ "${found}" == false ]]; then
            echo "missing stdlib conformance check for ${name}"
            exit 1
        fi
    done

    for checked in "${checked_names[@]}"; do
        if [[ ! -f "${examples_dir}/${checked}.flux" ]]; then
            echo "registered stdlib conformance example does not exist: ${checked}"
            exit 1
        fi
    done
}

check array '{"package":null,"imports":["array"],"results":[{"name":"_result","value":{"contains_two":true,"total":10,"doubled":[2,4,6,8],"all_positive":true,"any_large":true,"table_host":"edge-1"}}],"last":{"contains_two":true,"total":10,"doubled":[2,4,6,8],"all_positive":true,"any_large":true,"table_host":"edge-1"}}'
check csv '{"package":null,"imports":["csv"],"results":[{"name":"_result","value":{"host":"edge-2","value":"12"}}],"last":{"host":"edge-2","value":"12"}}'
check date '{"package":null,"imports":["date"],"results":[{"name":"_result","value":{"year":2024,"month":2,"day":3,"weekday":6,"hour":4,"minute":5,"second":6,"truncated":"{v: 2024-02-03T04:00:00Z}","added":"{v: 2024-02-03T06:05:06Z}","subtracted":"{v: 2024-02-03T04:00:06Z}"}}],"last":{"year":2024,"month":2,"day":3,"weekday":6,"hour":4,"minute":5,"second":6,"truncated":"{v: 2024-02-03T04:00:00Z}","added":"{v: 2024-02-03T06:05:06Z}","subtracted":"{v: 2024-02-03T04:00:06Z}"}}'
check dict '{"package":null,"imports":["dict"],"results":[{"name":"_result","value":{"edge2":65,"missing":-1,"removed":0,"literal_two":"two"}}],"last":{"edge2":65,"missing":-1,"removed":0,"literal_two":"two"}}'
check join '{"package":null,"imports":["array","join"],"results":[{"name":"_result","value":{"host":"edge-1","cpu":80.0,"mem":70.0,"inner":1,"left":2,"right":2,"full":3}}],"last":{"host":"edge-1","cpu":80.0,"mem":70.0,"inner":1,"left":2,"right":2,"full":3}}'
check json '{"package":null,"imports":["json"],"results":[{"name":"_result","value":"{\"host\":\"edge-1\",\"label\":\"edge \\\"one\\\"\",\"path\":\"cpu\\\\usage\",\"ok\":true,\"count\":3,\"values\":[1.5,false,\"cpu\"],\"at\":\"2024-01-01T00:00:00Z\"}"}],"last":"{\"host\":\"edge-1\",\"label\":\"edge \\\"one\\\"\",\"path\":\"cpu\\\\usage\",\"ok\":true,\"count\":3,\"values\":[1.5,false,\"cpu\"],\"at\":\"2024-01-01T00:00:00Z\"}"}'
check math '{"package":null,"imports":["math"],"results":[{"name":"_result","value":{"pi_round":3.14,"abs":4.5,"ceil":3.0,"floor":2.0,"pow":9.0,"sqrt":9.0}}],"last":{"pi_round":3.14,"abs":4.5,"ceil":3.0,"floor":2.0,"pow":9.0,"sqrt":9.0}}'
check regexp '{"package":null,"imports":["regexp"],"results":[{"name":"_result","value":{"match":true,"find":"cpu","quoted":"cpu\\.usage"}}],"last":{"match":true,"find":"cpu","quoted":"cpu\\.usage"}}'
check runtime '{"package":null,"imports":["runtime"],"results":[{"name":"_result","value":{"version":"playground-flux"}}],"last":{"version":"playground-flux"}}'
check sqlite '{"package":null,"imports":["sqlite"],"results":[{"name":"_result","value":{"host":"edge-1","value":71.5,"region":"west"}}],"last":{"host":"edge-1","value":71.5,"region":"west"}}'
check strings '{"package":null,"imports":["strings"],"results":[{"name":"_result","value":{"contains":true,"prefix":true,"suffix":true,"upper":"EDGE-1.CPU","lower":"cpu","joined":"edge-1/cpu","replaced":"node-1.cpu"}}],"last":{"contains":true,"prefix":true,"suffix":true,"upper":"EDGE-1.CPU","lower":"cpu","joined":"edge-1/cpu","replaced":"node-1.cpu"}}'
check_regex system '^\{"package":null,"imports":\["system"\],"results":\[\{"name":"_result","value":\{"now":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"\}\}\],"last":\{"now":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"\}\}$'
check types '{"package":null,"imports":["types"],"results":[{"name":"_result","value":{"int_numeric":true,"float_numeric":true,"string_numeric":false,"string_type":true,"bool_type":true,"time_type":true,"duration_type":true,"regexp_type":true,"bytes_type":false,"is_string":true,"is_int":true,"is_uint":true,"is_float":true,"is_bool":true,"is_time":true,"is_duration":true,"is_regexp":true,"string_is_duration":false}}],"last":{"int_numeric":true,"float_numeric":true,"string_numeric":false,"string_type":true,"bool_type":true,"time_type":true,"duration_type":true,"regexp_type":true,"bytes_type":false,"is_string":true,"is_int":true,"is_uint":true,"is_float":true,"is_bool":true,"is_time":true,"is_duration":true,"is_regexp":true,"string_is_duration":false}}'
check universe_aggregate '{"package":null,"imports":["array"],"results":[{"name":"_result","value":{"sum":6,"mean":4.0,"min":1,"max":3,"count":3,"spread":10.0,"quantile":17.5,"median":15.0,"first":10.0,"last":20.0,"top":20.0,"bottom":10.0,"reduced":45.0,"distinct_rows":1}}],"last":{"sum":6,"mean":4.0,"min":1,"max":3,"count":3,"spread":10.0,"quantile":17.5,"median":15.0,"first":10.0,"last":20.0,"top":20.0,"bottom":10.0,"reduced":45.0,"distinct_rows":1}}'
check universe_core '{"package":null,"imports":[],"results":[{"name":"_result","value":{"len_string":4,"len_array":3,"len_object":2,"string_time":"2024-01-01T00:00:00Z","contains_mem":true}}],"last":{"len_string":4,"len_array":3,"len_object":2,"string_time":"2024-01-01T00:00:00Z","contains_mem":true}}'
check universe_inspect '{"package":null,"imports":["array"],"results":[{"name":"_result","value":{"columns":["_time","host","region","_value"],"keys":["region"],"values":[10.0],"record_host":"edge-2","named_string":"<table bucket=\"inspect\" rows=2 tables=2>","plan":"<no plan>"}}],"last":{"columns":["_time","host","region","_value"],"keys":["region"],"values":[10.0],"record_host":"edge-2","named_string":"<table bucket=\"inspect\" rows=2 tables=2>","plan":"<no plan>"}}'
check universe_join '{"package":null,"imports":["array"],"results":[{"name":"_result","value":{"host":"edge-1","cpu":80.0,"mem":70.0}}],"last":{"host":"edge-1","cpu":80.0,"mem":70.0}}'
check universe_transform '{"package":null,"imports":["array"],"results":[{"name":"_result","value":{"row_host":"edge-1","row_service":"api","row_value":10.0,"row_score":11.0,"row_env":"prod","pivot_cpu":10.0,"pivot_mem":20.0,"union_rows":1}}],"last":{"row_host":"edge-1","row_service":"api","row_value":10.0,"row_score":11.0,"row_env":"prod","pivot_cpu":10.0,"pivot_mem":20.0,"union_rows":1}}'
check universe_window '{"package":null,"imports":["array"],"results":[{"name":"_result","value":{"elapsed":1,"difference":5.0,"derivative":5.0,"window_counts":[2,1],"aggregate_mean":12.5}}],"last":{"elapsed":1,"difference":5.0,"derivative":5.0,"window_counts":[2,1],"aggregate_mean":12.5}}'

assert_registry_complete
