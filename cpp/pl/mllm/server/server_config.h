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
// Created: 2026/09/13

#pragma once

#include <cstdint>
#include <string>

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
    // Max HTTP request body in bytes; larger bodies are rejected with 413
    // before parsing (a base64 image inflates ~4/3x in the JSON body and
    // again ~3x when expanded to RGB, so this caps total memory amplification).
    int64_t max_body_bytes = 64 * 1024 * 1024;
    // Max per-image decoded byte size; larger images are rejected with 400.
    int64_t max_image_bytes = 32 * 1024 * 1024;
};

} // namespace pl::mllm::server
