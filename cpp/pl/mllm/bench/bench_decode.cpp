// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/08/29 22:15

// End-to-end decode benchmark (SPEC §13.1). Loads a GGUF model through the
// Engine and reports prefill/decode throughput, peak RSS, backend and commit.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cpp/pl/mllm/bench/bench_common.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/loader/gguf.h"

using namespace pl::mllm;

namespace {

BackendKind parse_backend(const char* value) {
    return std::strcmp(value, "metal") == 0 ? BackendKind::kMetal : BackendKind::kCpu;
}

const char* backend_name(BackendKind kind) {
    return kind == BackendKind::kMetal ? "metal" : "cpu";
}

} // namespace

static void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s -m <model.gguf> [-p prompt] [-n max_tokens] "
                 "[--backend cpu|metal] [-t temp] [-s seed]\n",
                 prog);
}

static const char* dtype_name(DType dtype) {
    switch (dtype) {
        case DType::kF32:
            return "f32";
        case DType::kF16:
            return "f16";
        case DType::kQ8_0:
            return "q8_0";
        case DType::kQ4_0:
            return "q4_0";
    }
    return "unknown";
}

int main(int argc, char** argv) {
    std::string model_path;
    std::string prompt = "Hello";
    int32_t max_tokens = 64;
    float temperature = 0.0f;
    uint64_t seed = 1;
    BackendKind backend = BackendKind::kCpu;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prompt = argv[++i];
        } else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_tokens = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            temperature = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seed = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = parse_backend(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (model_path.empty()) {
        usage(argv[0]);
        return 1;
    }

    // Read quant type from the embedding tensor for reporting.
    std::string quant = "unknown";
    if (auto file = GGUFFile::Open(model_path); file.ok()) {
        if (auto emb = file.value()->tensor("token_embd.weight"); emb.ok()) {
            quant = dtype_name(emb.value().dtype());
        }
    }

    Engine::Options opts;
    opts.model_path = model_path;
    opts.backend = backend;

    auto engine_result = Engine::Create(opts);
    if (!engine_result.ok()) {
        std::fprintf(stderr, "error: %s\n", engine_result.status().message.c_str());
        return 1;
    }
    auto engine = std::move(engine_result).value();

    GenerateParams gp;
    gp.max_tokens = max_tokens;
    gp.temperature = temperature;
    gp.seed = seed;

    auto status =
        engine->GenerateStream(prompt, gp, [](std::string_view, int32_t) { return true; });
    if (!status.ok()) {
        std::fprintf(stderr, "generation error: %s\n", status.message.c_str());
        return 1;
    }

    const auto& perf = engine->last_perf_stats();

    // SPEC §13.1 output fields.
    std::printf("model_path       : %s\n", model_path.c_str());
    std::printf("quant_type       : %s\n", quant.c_str());
    std::printf("backend          : %s\n", backend_name(backend));
    std::printf("git_commit       : %s\n", bench::git_commit().c_str());
    std::printf("prompt_tokens    : %d\n", perf.prompt_tokens);
    std::printf("generated_tokens : %d\n", perf.generated_tokens);
    std::printf("prefill_ms       : %.2f\n", perf.prefill_ms);
    std::printf("decode_ms        : %.2f\n", perf.decode_ms);
    std::printf("total_ms         : %.2f\n", perf.total_ms);
    std::printf("tok_per_sec      : %.2f\n", perf.tok_per_sec);
    std::printf("ttft_ms          : %.2f\n", perf.time_to_first_token_ms);
    std::printf("peak_rss_mb      : %.2f\n",
                static_cast<double>(bench::peak_rss_bytes()) / (1024.0 * 1024.0));

    return 0;
}
