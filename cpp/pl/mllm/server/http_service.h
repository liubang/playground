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
// Created: 2026/09/12

#pragma once

#include <brpc/controller.h>
#include <cstdint>
#include <mutex>
#include <string>

#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/server/mllm.pb.h"

namespace pl::mllm::server {

// Static, rarely-changing knobs for the HTTP surface. Dynamic per-request
// parameters (temperature, max_tokens, ...) live in the request bodies.
struct ServerConfig {
    // Model id reported by /v1/models and echoed in chat-completion
    // responses (OpenAI clients send it back but a single-model server
    // ignores it).
    std::string model_name = "mllm";
    // Vocab piece marking an image slot in prompts (must match the engine's
    // Options::image_placeholder).
    std::string image_placeholder = "<|IMAGE_PLACEHOLDER|>";
    // Prompt scaffold for /v1/ocr. "{IMAGE}" expands to the
    // <|IMAGE_START|>placeholder<|IMAGE_END|> triple, "{TASK}" to the
    // request's task string. The default is the exact chat template
    // PaddleOCR-VL was trained behind (see its GGUF tokenizer.chat_template):
    // sending the bare task prefix puts the model off-distribution (never
    // emits EOS, hallucinates until the token cap).
    std::string ocr_template = "<|begin_of_sentence|>User: {IMAGE}{TASK}\nAssistant:\n";
    // Default task string for /v1/ocr when the request omits "prompt".
    std::string ocr_task = "OCR:";
    // Default generation cap when a request omits max_tokens.
    int32_t default_max_tokens = 2048;
    // Guardrail against pathological requests.
    int32_t max_images_per_request = 8;
};

// OpenAI-compatible(ish) HTTP API over a single Engine instance.
//
// Endpoints:
//   GET  /healthz                -> {"status":"ok"}
//   GET  /v1/models              -> OpenAI model list (single entry)
//   POST /v1/ocr                 -> {"image": "<base64|data-url>",
//                                    "prompt": "OCR:"?, "max_tokens": N?}
//                                   => {"text": "...", "usage": {...}, "perf": {...}}
//   POST /v1/chat/completions    -> OpenAI chat request (content parts may
//                                   include image_url with data: URLs);
//                                   non-streaming only for now.
//
// The engine is not thread-safe (and the GPU wouldn't benefit anyway), so
// all generations serialize on a mutex: concurrent requests queue rather
// than corrupt the KV cache.
class MllmHttpService : public proto::MllmHttpService {
public:
    MllmHttpService(Engine* engine, ServerConfig config);

    void default_method(google::protobuf::RpcController* controller,
                        const proto::HttpRequest* /*request*/,
                        proto::HttpResponse* /*response*/,
                        google::protobuf::Closure* done) override;

private:
    void HandleHealthz(brpc::Controller* cntl);
    void HandleListModels(brpc::Controller* cntl);
    void HandleOcr(brpc::Controller* cntl);
    void HandleChatCompletions(brpc::Controller* cntl);

    // Runs one generation under the engine mutex, collecting all streamed
    // pieces into `out`.
    Status Generate(const GenerateInput& input, GenerateParams params, std::string* out);

    Engine* engine_; // not owned; owned by main()
    ServerConfig config_;
    std::mutex engine_mu_;
};

} // namespace pl::mllm::server
