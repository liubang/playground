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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "cpp/pl/mllm/engine/engine.h"

using namespace pl::mllm;

static void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s -m <model.gguf> -p <prompt> [-n max_tokens] [-t temp] "
                 "[-s seed] [--backend cpu|metal] [--chat] [--system <msg>] "
                 "[--repeat-penalty <f>] [--top-k <k>] [--top-p <p>]\n",
                 prog);
}

int main(int argc, char** argv) {
    std::string model_path;
    std::string prompt;
    std::string system_msg;
    int32_t max_tokens = 128;
    int32_t top_k = 0;
    float temperature = 0.0f;
    float top_p = 1.0f;
    float repeat_penalty = 1.0f;
    uint64_t seed = 0;
    BackendKind backend = BackendKind::kCpu;
    bool chat_mode = false;

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
            const char* value = argv[++i];
            backend = std::strcmp(value, "metal") == 0 ? BackendKind::kMetal : BackendKind::kCpu;
        } else if (std::strcmp(argv[i], "--chat") == 0) {
            chat_mode = true;
        } else if (std::strcmp(argv[i], "--system") == 0 && i + 1 < argc) {
            system_msg = argv[++i];
            chat_mode = true;
        } else if (std::strcmp(argv[i], "--repeat-penalty") == 0 && i + 1 < argc) {
            repeat_penalty = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--top-k") == 0 && i + 1 < argc) {
            top_k = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--top-p") == 0 && i + 1 < argc) {
            top_p = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (model_path.empty() || prompt.empty()) {
        usage(argv[0]);
        return 1;
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
    gp.top_k = top_k;
    gp.top_p = top_p;
    gp.repeat_penalty = repeat_penalty;
    gp.seed = seed;

    // Chat mode: wrap the user message with the model's chat template
    // (Qwen ChatML / Llama-2 / Llama-3 families, detected from GGUF
    // metadata). Raw prompts pass through unchanged (embedding of the
    // assistant-turn opener makes generation continue as the assistant).
    std::string effective_prompt =
        chat_mode ? engine->FormatChatPrompt(prompt, system_msg) : prompt;

    std::string output;
    auto status =
        engine->GenerateStream(effective_prompt, gp, [&](std::string_view piece, int32_t /*tok*/) {
            std::cout << piece << std::flush;
            output.append(piece);
            return true;
        });

    std::cout << std::endl;

    if (!status.ok()) {
        std::fprintf(stderr, "\ngeneration error: %s\n", status.message.c_str());
        return 1;
    }

    // Print perf stats to stderr.
    const auto& perf = engine->last_perf_stats();
    std::fprintf(stderr,
                 "\n--- perf stats ---\n"
                 "prompt tokens : %d\n"
                 "generated     : %d\n"
                 "prefill ms    : %.2f\n"
                 "decode ms     : %.2f\n"
                 "total ms      : %.2f\n"
                 "tok/s         : %.2f\n"
                 "ttft ms       : %.2f\n",
                 perf.prompt_tokens,
                 perf.generated_tokens,
                 perf.prefill_ms,
                 perf.decode_ms,
                 perf.total_ms,
                 perf.tok_per_sec,
                 perf.time_to_first_token_ms);

    return 0;
}
