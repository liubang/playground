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
# Created: 2026/07/25

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "trace",
    srcs = [
        "config.go",
        "otel.go",
        "prompts.go",
        "recorder.go",
        "scores.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/trace",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "@io_opentelemetry_go_otel//:otel",
        "@io_opentelemetry_go_otel//attribute:attribute",
        "@io_opentelemetry_go_otel//codes:codes",
        "@io_opentelemetry_go_otel_exporters_otlp_otlptrace_otlptracehttp//:otlptracehttp",
        "@io_opentelemetry_go_otel_sdk//resource:resource",
        "@io_opentelemetry_go_otel_sdk//trace:trace",
        "@io_opentelemetry_go_otel_trace//:trace",
    ],
)

go_test(
    name = "trace_test",
    srcs = [
        "otel_test.go",
        "prompts_test.go",
    ],
    embed = [":trace"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "@io_opentelemetry_go_otel_sdk//trace:trace",
        "@io_opentelemetry_go_otel_sdk//trace/tracetest:tracetest",
    ],
)
