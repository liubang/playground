# Copyright (c) 2024 The Authors. All rights reserved.
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

load(
    "//cpp:copts/configure_copts.bzl",
    "LINKOPTS_WITH_ASAN",
    "TEST_COPTS",
)

_abseil_deps = [
    "@abseil-cpp//absl/algorithm:algorithm",
    "@abseil-cpp//absl/algorithm:container",
    "@abseil-cpp//absl/base:base",
    "@abseil-cpp//absl/base:config",
    "@abseil-cpp//absl/base:core_headers",
    "@abseil-cpp//absl/base:dynamic_annotations",
    "@abseil-cpp//absl/base:endian",
    "@abseil-cpp//absl/base:log_severity",
    "@abseil-cpp//absl/base:malloc_internal",
    "@abseil-cpp//absl/cleanup:cleanup",
    "@abseil-cpp//absl/cleanup:cleanup_internal",
    "@abseil-cpp//absl/container:btree",
    "@abseil-cpp//absl/container:common",
    "@abseil-cpp//absl/container:compressed_tuple",
    "@abseil-cpp//absl/container:container_memory",
    "@abseil-cpp//absl/container:fixed_array",
    "@abseil-cpp//absl/container:flat_hash_map",
    "@abseil-cpp//absl/container:flat_hash_set",
    "@abseil-cpp//absl/container:hash_policy_traits",
    "@abseil-cpp//absl/container:hashtable_debug",
    "@abseil-cpp//absl/container:hashtable_debug_hooks",
    "@abseil-cpp//absl/container:hashtablez_sampler",
    "@abseil-cpp//absl/container:inlined_vector",
    "@abseil-cpp//absl/container:inlined_vector_internal",
    "@abseil-cpp//absl/container:layout",
    "@abseil-cpp//absl/container:node_hash_map",
    "@abseil-cpp//absl/container:node_hash_set",
    "@abseil-cpp//absl/container:node_slot_policy",
    "@abseil-cpp//absl/container:raw_hash_map",
    "@abseil-cpp//absl/container:raw_hash_set",
    "@abseil-cpp//absl/container:tracked",
    "@abseil-cpp//absl/crc:crc32c",
    "@abseil-cpp//absl/debugging:failure_signal_handler",
    "@abseil-cpp//absl/debugging:leak_check",
    "@abseil-cpp//absl/debugging:stacktrace",
    "@abseil-cpp//absl/debugging:symbolize",
    "@abseil-cpp//absl/flags:commandlineflag",
    "@abseil-cpp//absl/flags:config",
    "@abseil-cpp//absl/flags:flag",
    "@abseil-cpp//absl/flags:marshalling",
    "@abseil-cpp//absl/flags:parse",
    "@abseil-cpp//absl/flags:reflection",
    "@abseil-cpp//absl/flags:usage",
    "@abseil-cpp//absl/functional:any_invocable",
    "@abseil-cpp//absl/functional:bind_front",
    "@abseil-cpp//absl/functional:function_ref",
    "@abseil-cpp//absl/hash:city",
    "@abseil-cpp//absl/hash:hash",
    "@abseil-cpp//absl/log:absl_check",
    "@abseil-cpp//absl/log:absl_log",
    "@abseil-cpp//absl/log:check",
    "@abseil-cpp//absl/log:die_if_null",
    "@abseil-cpp//absl/log:flags",
    "@abseil-cpp//absl/log:globals",
    "@abseil-cpp//absl/log:initialize",
    "@abseil-cpp//absl/log:log",
    "@abseil-cpp//absl/log:log_entry",
    "@abseil-cpp//absl/log:log_sink",
    "@abseil-cpp//absl/log:log_sink_registry",
    "@abseil-cpp//absl/log:log_streamer",
    "@abseil-cpp//absl/log:structured",
    "@abseil-cpp//absl/memory:memory",
    "@abseil-cpp//absl/meta:type_traits",
    "@abseil-cpp//absl/numeric:bits",
    "@abseil-cpp//absl/numeric:int128",
    "@abseil-cpp//absl/numeric:representation",
    "@abseil-cpp//absl/random:bit_gen_ref",
    "@abseil-cpp//absl/random:distributions",
    "@abseil-cpp//absl/random:random",
    "@abseil-cpp//absl/random:seed_gen_exception",
    "@abseil-cpp//absl/random:seed_sequences",
    "@abseil-cpp//absl/status:status",
    "@abseil-cpp//absl/status:statusor",
    "@abseil-cpp//absl/strings:cord",
    "@abseil-cpp//absl/strings:internal",
    "@abseil-cpp//absl/strings:str_format",
    "@abseil-cpp//absl/strings:strings",
    "@abseil-cpp//absl/synchronization:synchronization",
    "@abseil-cpp//absl/time:time",
    "@abseil-cpp//absl/time/internal/cctz:civil_time",
    "@abseil-cpp//absl/time/internal/cctz:time_zone",
    "@abseil-cpp//absl/types:any",
    "@abseil-cpp//absl/types:bad_any_cast",
    "@abseil-cpp//absl/types:bad_optional_access",
    "@abseil-cpp//absl/types:bad_variant_access",
    "@abseil-cpp//absl/types:compare",
    "@abseil-cpp//absl/types:optional",
    "@abseil-cpp//absl/types:span",
    "@abseil-cpp//absl/types:variant",
    "@abseil-cpp//absl/utility:utility",
]

[
    cc_test(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=c++20",
        ] + TEST_COPTS,
        linkopts = LINKOPTS_WITH_ASAN,
        deps = _abseil_deps + [
            "@googletest//:gtest_main",
        ],
    )
    for f in glob(["*.cpp"])
]
