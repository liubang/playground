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

// Pure, transport-free helpers behind MllmHttpService: base64/data-URL
// decoding, OCR prompt templating, and request parsing/validation. Kept in
// a separate unit from the brpc-facing service so they are unit-testable
// without a Controller, socket, or model.

#include <cstdint>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/server/server_config.h"

namespace pl::mllm::server {

// ---------------------------------------------------------------------------
// base64 / data-URL
// ---------------------------------------------------------------------------

// Decodes standard base64, strict about framing: whitespace is tolerated
// anywhere, but nothing except '=' and whitespace may follow the first '='
// padding character, and the leftover-bit tail must either be empty or a
// canonical zero padding tail (2 or 4 bits). Returns false on invalid
// characters, a truncated quantum, or non-canonical padding bits.
[[nodiscard]] bool Base64Decode(std::string_view in, std::vector<uint8_t>* out);

// Accepts a raw base64 blob or a data URL ("data:image/png;base64,...."),
// returning the decoded bytes. A data URL without the base64 marker still
// reaches the decoder (and fails there), so the error is uniform.
[[nodiscard]] bool DecodeImagePayload(std::string_view payload, std::vector<uint8_t>* out);

// ---------------------------------------------------------------------------
// OCR prompt templating
// ---------------------------------------------------------------------------

// Expands every {IMAGE} and {TASK} marker in `prompt_template`. Values are
// substituted left-to-right and the scan position advances past each
// inserted value, so a task string that itself contains "{TASK}" is never
// re-expanded (and cannot loop).
[[nodiscard]] std::string ExpandOcrTemplate(std::string_view prompt_template,
                                            std::string_view image_slot,
                                            std::string_view task);

// ---------------------------------------------------------------------------
// Request parsing + validation
// ---------------------------------------------------------------------------

// ErrorCode contract shared by both parsers:
//   kInvalidArgument -> malformed request (HTTP 400)
//   kUnsupported     -> well-formed but unsupported feature (stream,
//                       multi-turn) (HTTP 400, "unsupported_parameter")

struct ParsedOcrRequest {
    std::string image_payload; // base64 or data URL, NOT yet decoded
    std::string task;
    int32_t max_tokens = 0; // clamped to [1, kMaxTokensCap]
};

[[nodiscard]] Result<ParsedOcrRequest> ParseOcrRequest(const simdjson::dom::element& root,
                                                       const ServerConfig& config);

// Hard ceiling for max_tokens in any request path (guardrail; raise only
// with a KV-capacity argument).
inline constexpr int32_t kMaxTokensCap = 16384;

struct ParsedChatRequest {
    std::string system;    // all system messages concatenated with '\n'
    std::string user_text; // all user text parts concatenated with '\n'
    // Undecoded image payloads (base64/data URLs) from image_url parts, in
    // document order. Decoding is left to the caller so a text-only engine
    // can reject (503) before paying for base64/image decode.
    std::vector<std::string> image_payloads;
    GenerateParams params;
};

// Parses an OpenAI chat request. Multi-turn history (assistant/tool roles)
// and stream:true are rejected with kUnsupported instead of being silently
// dropped: every OpenAI client carries history, so answering from a
// collapsed prompt without telling the caller was worse than failing fast.
[[nodiscard]] Result<ParsedChatRequest> ParseChatRequest(const simdjson::dom::element& root,
                                                         const ServerConfig& config);

// OpenAI finish_reason: "length" iff the generation hit the token cap
// (max_tokens is the effective, already-clamped cap of the request), else
// "stop" (EOS, or nothing generated).
[[nodiscard]] inline const char* FinishReasonFor(const PerfStats& perf, int32_t max_tokens) {
    return (max_tokens > 0 && perf.generated_tokens >= max_tokens) ? "length" : "stop";
}

} // namespace pl::mllm::server
