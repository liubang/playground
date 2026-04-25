#!/bin/bash
set -euo pipefail

workspace_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
flux_bin="${workspace_root}/cpp/pl/flux/flux"
examples_dir="${workspace_root}/cpp/pl/flux/examples/stdlib_conformance"

check() {
    local name="$1"
    local expected="$2"
    local output
    output="$("${flux_bin}" --output-format json "${examples_dir}/${name}.flux")"
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
    output="$("${flux_bin}" --output-format json "${examples_dir}/${name}.flux")"
    if [[ ! "${output}" =~ ${pattern} ]]; then
        echo "unexpected stdlib conformance output for ${name}:"
        printf 'pattern: %s\n' "${pattern}"
        printf 'actual:  %s\n' "${output}"
        exit 1
    fi
}

check array '{"package":null,"imports":["array"],"results":[{"name":"numbers","value":[1,2,3]},{"name":"doubled","value":[2,4,6]},{"name":"_result","value":{"contains_two":true,"total":6,"doubled":[2,4,6],"all_positive":true,"any_large":true}}],"last":{"contains_two":true,"total":6,"doubled":[2,4,6],"all_positive":true,"any_large":true}}'
check csv '{"package":null,"imports":["csv"],"results":[{"name":"row","value":{"_time":"2024-01-01T00:01:00Z","host":"edge-2","_value":"12"}},{"name":"_result","value":{"host":"edge-2","value":"12"}}],"last":{"host":"edge-2","value":"12"}}'
check date '{"package":null,"imports":["date"],"results":[{"name":"t","value":"2024-02-03T04:05:06Z"},{"name":"_result","value":{"year":2024,"month":2,"day":3,"weekday":6,"hour":4,"minute":5,"second":6,"truncated":"{v: 2024-02-03T04:00:00Z}","added":"{v: 2024-02-03T06:05:06Z}","subtracted":"{v: 2024-02-03T04:00:06Z}"}}],"last":{"year":2024,"month":2,"day":3,"weekday":6,"hour":4,"minute":5,"second":6,"truncated":"{v: 2024-02-03T04:00:00Z}","added":"{v: 2024-02-03T06:05:06Z}","subtracted":"{v: 2024-02-03T04:00:06Z}"}}'
check dict '{"package":null,"imports":["dict"],"results":[{"name":"base","value":{"edge-1":80,"edge-2":60}},{"name":"updated","value":{"edge-1":80,"edge-2":60,"edge-3":70}},{"name":"overwritten","value":{"edge-1":80,"edge-2":65,"edge-3":70}},{"name":"trimmed","value":{"edge-2":65,"edge-3":70}},{"name":"literal","value":{"1":"one","2":"two"}},{"name":"_result","value":{"edge2":65,"missing":-1,"removed":0,"literal_two":"two"}}],"last":{"edge2":65,"missing":-1,"removed":0,"literal_two":"two"}}'
check json '{"package":null,"imports":["json"],"results":[{"name":"_result","value":"{\"host\":\"edge-1\",\"label\":\"edge \\\"one\\\"\",\"path\":\"cpu\\\\usage\",\"ok\":true,\"count\":3,\"values\":[1.5,false,\"cpu\"],\"at\":\"2024-01-01T00:00:00Z\"}"}],"last":"{\"host\":\"edge-1\",\"label\":\"edge \\\"one\\\"\",\"path\":\"cpu\\\\usage\",\"ok\":true,\"count\":3,\"values\":[1.5,false,\"cpu\"],\"at\":\"2024-01-01T00:00:00Z\"}"}'
check join '{"package":null,"imports":["array","join"],"results":[{"name":"left","value":{"type":"table","bucket":"array","rows":[{"host":"edge-1","cpu":80.0},{"host":"edge-2","cpu":60.0}],"tables":[{"columns":["host","cpu"],"rows":[{"host":"edge-1","cpu":80.0},{"host":"edge-2","cpu":60.0}]}]}},{"name":"right","value":{"type":"table","bucket":"array","rows":[{"host":"edge-1","mem":70.0},{"host":"edge-3","mem":50.0}],"tables":[{"columns":["host","mem"],"rows":[{"host":"edge-1","mem":70.0},{"host":"edge-3","mem":50.0}]}]}},{"name":"row","value":{"host":"edge-1","cpu":80.0,"mem":70.0}},{"name":"_result","value":{"host":"edge-1","cpu":80.0,"mem":70.0}}],"last":{"host":"edge-1","cpu":80.0,"mem":70.0}}'
check math '{"package":null,"imports":["math"],"results":[{"name":"_result","value":{"pi_round":3.14,"abs":4.5,"ceil":3.0,"floor":2.0,"pow":9.0,"sqrt":9.0}}],"last":{"pi_round":3.14,"abs":4.5,"ceil":3.0,"floor":2.0,"pow":9.0,"sqrt":9.0}}'
check regexp '{"package":null,"imports":["regexp"],"results":[{"name":"r","value":"edge-[0-9]+"},{"name":"_result","value":{"match":true,"find":"cpu","quoted":"cpu\\.usage"}}],"last":{"match":true,"find":"cpu","quoted":"cpu\\.usage"}}'
check runtime '{"package":null,"imports":["runtime"],"results":[{"name":"_result","value":{"version":"playground-flux"}}],"last":{"version":"playground-flux"}}'
check strings '{"package":null,"imports":["strings"],"results":[{"name":"raw","value":"  edge-1.cpu  "},{"name":"trimmed","value":"edge-1.cpu"},{"name":"_result","value":{"contains":true,"prefix":true,"suffix":true,"upper":"EDGE-1.CPU","lower":"cpu","joined":"edge-1/cpu","replaced":"node-1.cpu"}}],"last":{"contains":true,"prefix":true,"suffix":true,"upper":"EDGE-1.CPU","lower":"cpu","joined":"edge-1/cpu","replaced":"node-1.cpu"}}'
check_regex system '^\{"package":null,"imports":\["system"\],"results":\[\{"name":"_result","value":\{"now":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"\}\}\],"last":\{"now":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"\}\}$'
check types '{"package":null,"imports":["types"],"results":[{"name":"_result","value":{"int_numeric":true,"float_numeric":true,"string_numeric":false,"string_type":true,"bool_type":true,"time_type":true,"duration_type":true,"regexp_type":true,"bytes_type":false,"is_string":true,"is_int":true,"is_uint":true,"is_float":true,"is_bool":true,"is_time":true,"is_duration":true,"is_regexp":true,"string_is_duration":false}}],"last":{"int_numeric":true,"float_numeric":true,"string_numeric":false,"string_type":true,"bool_type":true,"time_type":true,"duration_type":true,"regexp_type":true,"bytes_type":false,"is_string":true,"is_int":true,"is_uint":true,"is_float":true,"is_bool":true,"is_time":true,"is_duration":true,"is_regexp":true,"string_is_duration":false}}'
check universe '{"package":null,"imports":[],"results":[{"name":"data","value":{"type":"table","bucket":"conformance","rows":[{"_time":"2024-01-01T00:00:00Z","host":"edge-1","_value":10.0},{"_time":"2024-01-01T00:01:00Z","host":"edge-1","_value":15.0},{"_time":"2024-01-01T00:02:00Z","host":"edge-2","_value":20.0}],"tables":[{"columns":["_time","host","_value"],"rows":[{"_time":"2024-01-01T00:00:00Z","host":"edge-1","_value":10.0},{"_time":"2024-01-01T00:01:00Z","host":"edge-1","_value":15.0},{"_time":"2024-01-01T00:02:00Z","host":"edge-2","_value":20.0}]}]}},{"name":"row","value":{"host":"edge-1","_group":{"host":"edge-1"},"_value":2}},{"name":"_result","value":{"host":"edge-1","count":2}}],"last":{"host":"edge-1","count":2}}'
