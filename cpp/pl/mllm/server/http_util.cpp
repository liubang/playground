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

#include "cpp/pl/mllm/server/http_util.h"

#include <algorithm>
#include <array>
#include <utility>

namespace pl::mllm::server {

namespace {

// ---------------------------------------------------------------------------
// simdjson field readers (all optional-field tolerant)
// ---------------------------------------------------------------------------

bool ReadString(const simdjson::dom::element& obj, std::string_view key, std::string* out) {
    std::string_view sv;
    if (obj.at_key(key).get(sv) == simdjson::SUCCESS) {
        *out = std::string(sv);
        return true;
    }
    return false;
}

bool ReadInt(const simdjson::dom::element& obj, std::string_view key, int64_t* out) {
    int64_t v = 0;
    if (obj.at_key(key).get(v) == simdjson::SUCCESS) {
        *out = v;
        return true;
    }
    return false;
}

bool ReadDouble(const simdjson::dom::element& obj, std::string_view key, double* out) {
    double v = 0.0;
    if (obj.at_key(key).get(v) == simdjson::SUCCESS) {
        *out = v;
        return true;
    }
    // OpenAI clients happily send integer literals (e.g. "temperature": 1).
    int64_t iv = 0;
    if (obj.at_key(key).get(iv) == simdjson::SUCCESS) {
        *out = static_cast<double>(iv);
        return true;
    }
    return false;
}

bool ReadBool(const simdjson::dom::element& obj, std::string_view key, bool* out) {
    bool v = false;
    if (obj.at_key(key).get(v) == simdjson::SUCCESS) {
        *out = v;
        return true;
    }
    return false;
}

Status InvalidArgument(std::string message) {
    return Status::Error(ErrorCode::kInvalidArgument, std::move(message));
}

Status Unsupported(std::string message) {
    return Status::Error(ErrorCode::kUnsupported, std::move(message));
}

} // namespace

bool Base64Decode(std::string_view in, std::vector<uint8_t>* out) {
    static const std::array<int8_t, 256> kTable = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 26; ++i) {
            t[static_cast<size_t>('A' + i)] = static_cast<int8_t>(i);
            t[static_cast<size_t>('a' + i)] = static_cast<int8_t>(26 + i);
        }
        for (int i = 0; i < 10; ++i) {
            t[static_cast<size_t>('0' + i)] = static_cast<int8_t>(52 + i);
        }
        t[static_cast<size_t>('+')] = 62;
        t[static_cast<size_t>('/')] = 63;
        return t;
    }();

    out->clear();
    out->reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    bool seen_padding = false;
    for (const char c : in) {
        const auto uc = static_cast<unsigned char>(c);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '=') {
            seen_padding = true;
            continue; // the quantum boundary check below rejects truncation
        }
        if (seen_padding) {
            // Strict framing: base64 carries no payload after the first '='.
            return false;
        }
        const int8_t v = kTable[uc];
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits must be a whole canonical padding tail (0, 2 or 4 bits of
    // zeros): a 6-bit leftover is a truncated quantum, and non-zero tail bits
    // are non-canonical encodings.
    if (bits >= 6 || ((acc & ((1u << bits) - 1u)) != 0)) {
        return false;
    }
    return true;
}

bool DecodeImagePayload(std::string_view payload, std::vector<uint8_t>* out) {
    if (payload.starts_with("data:")) {
        const size_t comma = payload.find(',');
        if (comma == std::string_view::npos) {
            return false;
        }
        payload = payload.substr(comma + 1);
    }
    return Base64Decode(payload, out);
}

std::string ExpandOcrTemplate(std::string_view prompt_template,
                              std::string_view image_slot,
                              std::string_view task) {
    std::string prompt(prompt_template);
    for (const auto& [marker, value] : {std::pair{std::string_view{"{IMAGE}"}, image_slot},
                                        std::pair{std::string_view{"{TASK}"}, task}}) {
        size_t pos = 0;
        while ((pos = prompt.find(marker, pos)) != std::string::npos) {
            prompt.replace(pos, marker.size(), value);
            // Skip past the inserted value: never re-expand markers that the
            // task (or slot) text itself contains.
            pos += value.size();
        }
    }
    return prompt;
}

Result<ParsedOcrRequest> ParseOcrRequest(const simdjson::dom::element& root,
                                         const ServerConfig& config) {
    if (!root.is_object()) {
        return InvalidArgument("request body is not a JSON object");
    }
    ParsedOcrRequest parsed;
    if (!ReadString(root, "image", &parsed.image_payload) || parsed.image_payload.empty()) {
        return InvalidArgument("missing required field \"image\" (base64 or data URL)");
    }
    parsed.task = config.ocr_task;
    ReadString(root, "prompt", &parsed.task);
    int64_t max_tokens = config.default_max_tokens;
    ReadInt(root, "max_tokens", &max_tokens);
    parsed.max_tokens = static_cast<int32_t>(std::clamp<int64_t>(max_tokens, 1, kMaxTokensCap));
    return parsed;
}

Result<ParsedChatRequest> ParseChatRequest(const simdjson::dom::element& root,
                                           const ServerConfig& config) {
    if (!root.is_object()) {
        return InvalidArgument("request body is not a JSON object");
    }
    ParsedChatRequest parsed;

    bool stream = false;
    ReadBool(root, "stream", &stream);
    if (stream) {
        return Unsupported("\"stream\": true is not supported yet");
    }

    simdjson::dom::array messages;
    if (root.at_key("messages").get(messages) != simdjson::SUCCESS) {
        return InvalidArgument("missing required field \"messages\"");
    }

    // Collapse the conversation into one user turn: concatenate all system
    // messages into `system` and all user texts (in order) into `user_text`,
    // collecting image payloads from every content part. History roles
    // (assistant/tool) are rejected rather than silently dropped.
    for (const simdjson::dom::element msg : messages) {
        std::string_view role;
        if (msg.at_key("role").get(role) != simdjson::SUCCESS) {
            continue;
        }
        if (role == "assistant" || role == "tool") {
            return Unsupported("multi-turn conversations (\"assistant\"/\"tool\" roles) are not "
                               "supported yet; send a single user turn");
        }
        simdjson::dom::element content;
        if (msg.at_key("content").get(content) != simdjson::SUCCESS) {
            continue;
        }

        const auto append_text = [&](std::string_view text) {
            std::string* dst = nullptr;
            if (role == "system") {
                dst = &parsed.system;
            } else if (role == "user") {
                dst = &parsed.user_text;
            }
            if (dst == nullptr) {
                return;
            }
            if (!dst->empty()) {
                *dst += '\n';
            }
            *dst += text;
        };

        if (content.is_string()) {
            append_text(content.get_string().value());
            continue;
        }
        if (!content.is_array()) {
            continue;
        }
        // Bind the content array to a NAMED variable: binding
        // `simdjson_result<dom::array>::value() &&` into a range-for would
        // reference a dom::array inside the dead temporary result object —
        // a real use-after-scope (caught by ASan) even though the raw handle
        // is otherwise trivially copyable.
        simdjson::dom::array parts;
        if (content.get_array().get(parts) != simdjson::SUCCESS) {
            continue;
        }
        for (const simdjson::dom::element part : parts) {
            std::string_view type;
            if (part.at_key("type").get(type) != simdjson::SUCCESS) {
                continue;
            }
            if (type == "text") {
                std::string_view text;
                if (part.at_key("text").get(text) == simdjson::SUCCESS) {
                    append_text(text);
                }
            } else if (type == "image_url" && role == "user") {
                // OpenAI shape: {"type":"image_url","image_url":{"url":"data:..."}}
                // (a bare string instead of the object is also accepted).
                simdjson::dom::element iu;
                std::string_view url;
                if (part.at_key("image_url").get(iu) != simdjson::SUCCESS) {
                    continue;
                }
                if (iu.is_string()) {
                    url = iu.get_string().value();
                } else if (iu.at_key("url").get(url) != simdjson::SUCCESS) {
                    continue;
                }
                if (static_cast<int32_t>(parsed.image_payloads.size()) >=
                    config.max_images_per_request) {
                    return InvalidArgument("too many images in one request");
                }
                parsed.image_payloads.emplace_back(url);
            }
        }
    }

    if (parsed.user_text.empty() && parsed.image_payloads.empty()) {
        return InvalidArgument("no user content found in \"messages\"");
    }

    int64_t max_tokens = 0;
    if (ReadInt(root, "max_tokens", &max_tokens) ||
        ReadInt(root, "max_completion_tokens", &max_tokens)) {
        parsed.params.max_tokens =
            static_cast<int32_t>(std::clamp<int64_t>(max_tokens, 1, kMaxTokensCap));
    } else {
        parsed.params.max_tokens = config.default_max_tokens;
    }

    // Sampling parameters are validated, not silently coerced: an out-of-range
    // value almost always means a buggy client, and the sampler's behavior
    // there is undefined.
    double f = 0.0;
    if (ReadDouble(root, "temperature", &f)) {
        if (f < 0.0 || f > 2.0) {
            return InvalidArgument("\"temperature\" must be in [0, 2]");
        }
        parsed.params.temperature = static_cast<float>(f);
    }
    if (ReadDouble(root, "top_p", &f)) {
        if (f <= 0.0 || f > 1.0) {
            return InvalidArgument("\"top_p\" must be in (0, 1]");
        }
        parsed.params.top_p = static_cast<float>(f);
    }
    int64_t top_k = 0;
    if (ReadInt(root, "top_k", &top_k)) {
        if (top_k < 0) {
            return InvalidArgument("\"top_k\" must be >= 0");
        }
        parsed.params.top_k = static_cast<int32_t>(top_k);
    }
    int64_t seed = 0;
    if (ReadInt(root, "seed", &seed)) {
        parsed.params.seed = static_cast<uint64_t>(seed);
    }
    return parsed;
}

} // namespace pl::mllm::server
