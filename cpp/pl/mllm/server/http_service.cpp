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

#include "cpp/pl/mllm/server/http_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <butil/logging.h>
#include <chrono>
#include <cstdio>
#include <simdjson.h>
#include <string>
#include <utility>
#include <vector>

#include "cpp/pl/mllm/media/image_io.h"

namespace pl::mllm::server {

namespace {

// ---------------------------------------------------------------------------
// JSON builders — simdjson::builder::string_builder handles escaping and
// number formatting; `builtin` is the best statically-available
// implementation (public, stable alias).
// ---------------------------------------------------------------------------

using JsonBuilder = simdjson::builtin::builder::string_builder;

void SendJson(brpc::Controller* cntl, int status, std::string body) {
    cntl->http_response().set_status_code(status);
    cntl->http_response().set_content_type("application/json");
    cntl->response_attachment().append(std::move(body));
}

// Serializes the builder content and sends it. The builder's only failure
// mode is buffer allocation, mapped to a 500.
void SendBuiltJson(brpc::Controller* cntl, int status, JsonBuilder* sb) {
    std::string_view sv;
    if (sb->view().get(sv) != simdjson::SUCCESS) {
        SendJson(
            cntl,
            500,
            R"({"error":{"message":"json buffer allocation failed","type":"server_error","code":null}})");
        return;
    }
    SendJson(cntl, status, std::string(sv));
}

// OpenAI-style error body: {"error":{"message":...,"type":...,"code":null}}.
void SendError(brpc::Controller* cntl,
               int status,
               std::string_view message,
               std::string_view type = "invalid_request_error") {
    JsonBuilder sb;
    sb.start_object();
    sb.escape_and_append_with_quotes("error");
    sb.append_colon();
    sb.start_object();
    sb.append_key_value("message", message);
    sb.append_comma();
    sb.append_key_value("type", type);
    sb.append_comma();
    sb.append_key_value("code", nullptr);
    sb.end_object();
    sb.end_object();
    SendBuiltJson(cntl, status, &sb);
}

int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string MakeCompletionId() {
    static std::atomic<uint64_t> counter{0};
    std::array<char, 40> buf{};
    std::snprintf(buf.data(),
                  buf.size(),
                  "chatcmpl-%llx%06llx",
                  static_cast<unsigned long long>(NowUnixSeconds()),
                  static_cast<unsigned long long>(counter.fetch_add(1) & 0xFFFFFF));
    return buf.data();
}

// ---------------------------------------------------------------------------
// base64 / data-URL helpers
// ---------------------------------------------------------------------------

// Decodes standard base64 (whitespace tolerated). Returns false on invalid
// characters or truncated quantum.
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
    for (const char c : in) {
        const auto uc = static_cast<unsigned char>(c);
        if (c == '=') {
            break; // padding: the quantum boundary check below rejects truncation
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
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
    // Leftover bits must be a whole padding tail (2 or 4 bits), all zero.
    if (bits >= 6 || ((acc & ((1u << bits) - 1u)) != 0)) {
        return false;
    }
    return true;
}

// Accepts a raw base64 blob or a data URL ("data:image/png;base64,...."),
// returning the decoded bytes.
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

// Appends "usage":{...} (OpenAI token accounting, from the engine's perf stats).
void AppendUsage(JsonBuilder* sb, const PerfStats& perf) {
    sb->escape_and_append_with_quotes("usage");
    sb->append_colon();
    sb->start_object();
    sb->append_key_value("prompt_tokens", perf.prompt_tokens);
    sb->append_comma();
    sb->append_key_value("completion_tokens", perf.generated_tokens);
    sb->append_comma();
    sb->append_key_value("total_tokens", perf.prompt_tokens + perf.generated_tokens);
    sb->end_object();
}

// Appends "perf":{...} (mllm-specific latency/throughput detail).
void AppendPerf(JsonBuilder* sb, const PerfStats& perf) {
    sb->escape_and_append_with_quotes("perf");
    sb->append_colon();
    sb->start_object();
    sb->append_key_value("prefill_ms", perf.prefill_ms);
    sb->append_comma();
    sb->append_key_value("decode_ms", perf.decode_ms);
    sb->append_comma();
    sb->append_key_value("total_ms", perf.total_ms);
    sb->append_comma();
    sb->append_key_value("tok_per_sec", perf.tok_per_sec);
    sb->append_comma();
    sb->append_key_value("time_to_first_token_ms", perf.time_to_first_token_ms);
    sb->end_object();
}

} // namespace

MllmHttpService::MllmHttpService(Engine* engine, ServerConfig config)
    : engine_(engine), config_(std::move(config)) {}

void MllmHttpService::default_method(google::protobuf::RpcController* controller,
                                     const proto::HttpRequest* /*request*/,
                                     proto::HttpResponse* /*response*/,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);

    const brpc::HttpMethod method = cntl->http_request().method();
    const std::string path = cntl->http_request().uri().path();

    if (method == brpc::HTTP_METHOD_GET && path == "/healthz") {
        HandleHealthz(cntl);
    } else if (method == brpc::HTTP_METHOD_GET && path == "/v1/models") {
        HandleListModels(cntl);
    } else if (method == brpc::HTTP_METHOD_POST && path == "/v1/ocr") {
        HandleOcr(cntl);
    } else if (method == brpc::HTTP_METHOD_POST && path == "/v1/chat/completions") {
        HandleChatCompletions(cntl);
    } else {
        SendError(cntl, 404, "unknown route: " + path, "not_found");
    }
}

void MllmHttpService::HandleHealthz(brpc::Controller* cntl) {
    SendJson(cntl, 200, R"({"status":"ok"})");
}

void MllmHttpService::HandleListModels(brpc::Controller* cntl) {
    JsonBuilder sb;
    sb.start_object();
    sb.append_key_value("object", "list");
    sb.append_comma();
    sb.escape_and_append_with_quotes("data");
    sb.append_colon();
    sb.start_array();
    sb.start_object();
    sb.append_key_value("id", config_.model_name);
    sb.append_comma();
    sb.append_key_value("object", "model");
    sb.append_comma();
    sb.append_key_value("created", NowUnixSeconds());
    sb.append_comma();
    sb.append_key_value("owned_by", "mllm");
    sb.end_object();
    sb.end_array();
    sb.end_object();
    SendBuiltJson(cntl, 200, &sb);
}

Status MllmHttpService::Generate(const GenerateInput& input,
                                 GenerateParams params,
                                 std::string* out) {
    std::lock_guard<std::mutex> lock(engine_mu_);
    out->clear();
    return engine_->GenerateStream(input, params, [out](std::string_view piece, int32_t) {
        out->append(piece);
        return true;
    });
}

void MllmHttpService::HandleOcr(brpc::Controller* cntl) {
    if (!engine_->has_vision()) {
        SendError(cntl, 503, "engine has no vision tower (start with --mmproj)", "server_error");
        return;
    }

    const std::string body_str = cntl->request_attachment().to_string();
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(body_str).get(root) != simdjson::SUCCESS || !root.is_object()) {
        SendError(cntl, 400, "request body is not valid JSON");
        return;
    }

    std::string image_payload;
    if (!ReadString(root, "image", &image_payload) || image_payload.empty()) {
        SendError(cntl, 400, "missing required field \"image\" (base64 or data URL)");
        return;
    }
    std::string task = config_.ocr_task;
    ReadString(root, "prompt", &task);
    int64_t max_tokens = config_.default_max_tokens;
    ReadInt(root, "max_tokens", &max_tokens);

    std::vector<uint8_t> image_bytes;
    if (!DecodeImagePayload(image_payload, &image_bytes) || image_bytes.empty()) {
        SendError(cntl, 400, "invalid base64 image payload");
        return;
    }
    auto image = media::LoadImageData(image_bytes.data(), image_bytes.size());
    if (!image.ok()) {
        SendError(cntl, 400, "cannot decode image: " + image.status().message);
        return;
    }

    // Expand the OCR prompt scaffold: {IMAGE} -> the START/placeholder/END
    // triple (llama.cpp mtmd layout), {TASK} -> the request's task string.
    std::string prompt = config_.ocr_template;
    const std::string image_slot = "<|IMAGE_START|>" + config_.image_placeholder + "<|IMAGE_END|>";
    for (const auto& [marker, value] :
         {std::pair{std::string_view{"{IMAGE}"}, std::string_view{image_slot}},
          std::pair{std::string_view{"{TASK}"}, std::string_view{task}}}) {
        size_t pos = 0;
        while ((pos = prompt.find(marker, pos)) != std::string::npos) {
            prompt.replace(pos, marker.size(), value);
            pos += value.size();
        }
    }

    GenerateParams params;
    params.max_tokens = static_cast<int32_t>(std::clamp<int64_t>(max_tokens, 1, 16384));

    GenerateInput input;
    input.prompt = std::move(prompt);
    input.images.push_back(std::move(image).value());

    std::string text;
    const Status status = Generate(input, params, &text);
    if (!status.ok()) {
        SendError(cntl, 500, "generation failed: " + status.message, "server_error");
        return;
    }

    const PerfStats perf = engine_->last_perf_stats();
    JsonBuilder sb;
    sb.start_object();
    sb.append_key_value("text", text);
    sb.append_comma();
    sb.append_key_value("model", config_.model_name);
    sb.append_comma();
    AppendUsage(&sb, perf);
    sb.append_comma();
    AppendPerf(&sb, perf);
    sb.end_object();
    SendBuiltJson(cntl, 200, &sb);
}

void MllmHttpService::HandleChatCompletions(brpc::Controller* cntl) {
    const std::string body_str = cntl->request_attachment().to_string();
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(body_str).get(root) != simdjson::SUCCESS || !root.is_object()) {
        SendError(cntl, 400, "request body is not valid JSON");
        return;
    }

    bool stream = false;
    ReadBool(root, "stream", &stream);
    if (stream) {
        // SSE needs chunked progressive responses; queued behind OCR support.
        SendError(cntl, 400, "\"stream\": true is not supported yet", "unsupported_parameter");
        return;
    }

    simdjson::dom::array messages;
    if (root.at_key("messages").get(messages) != simdjson::SUCCESS) {
        SendError(cntl, 400, "missing required field \"messages\"");
        return;
    }

    // Collapse the conversation into one user turn: concatenate all system
    // messages into `system`, all user texts (in order) into `user_text`,
    // and collect images from every content part. (Single-turn models like
    // PaddleOCR-VL only ever see this flattened prompt; multi-turn chat
    // templating is a later refinement.)
    std::string system;
    std::string user_text;
    std::vector<media::Image> images;
    std::vector<std::vector<uint8_t>> image_bytes; // backing storage alive until decode done

    for (const simdjson::dom::element msg : messages) {
        std::string_view role;
        if (msg.at_key("role").get(role) != simdjson::SUCCESS) {
            continue;
        }
        simdjson::dom::element content;
        if (msg.at_key("content").get(content) != simdjson::SUCCESS) {
            continue;
        }

        const auto append_text = [&](std::string_view text) {
            if (role == "system") {
                if (!system.empty()) {
                    system += '\n';
                }
                system += text;
            } else if (role == "user") {
                if (!user_text.empty()) {
                    user_text += '\n';
                }
                user_text += text;
            }
        };

        if (content.is_string()) {
            append_text(content.get_string().value());
            continue;
        }
        if (!content.is_array()) {
            continue;
        }
        for (const simdjson::dom::element part : content.get_array().value()) {
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
                if (static_cast<int32_t>(images.size()) >= config_.max_images_per_request) {
                    SendError(cntl, 400, "too many images in one request");
                    return;
                }
                std::vector<uint8_t> bytes;
                if (!DecodeImagePayload(url, &bytes) || bytes.empty()) {
                    SendError(cntl, 400, "invalid image_url payload (expecting a data: URL)");
                    return;
                }
                image_bytes.push_back(std::move(bytes));
                const auto& back = image_bytes.back();
                auto img = media::LoadImageData(back.data(), back.size());
                if (!img.ok()) {
                    SendError(cntl, 400, "cannot decode image: " + img.status().message);
                    return;
                }
                images.push_back(std::move(img).value());
            }
        }
    }

    if (user_text.empty() && images.empty()) {
        SendError(cntl, 400, "no user content found in \"messages\"");
        return;
    }
    if (!images.empty() && !engine_->has_vision()) {
        SendError(cntl, 503, "engine has no vision tower (start with --mmproj)", "server_error");
        return;
    }

    // Splice images into the user turn before templating, so the visual
    // tokens land inside the model's trained "User: <image>..." layout.
    if (!images.empty() && user_text.find(config_.image_placeholder) == std::string::npos) {
        std::string prefixed;
        for (size_t i = 0; i < images.size(); ++i) {
            prefixed += "<|IMAGE_START|>" + config_.image_placeholder + "<|IMAGE_END|>";
        }
        user_text = prefixed + user_text;
    }
    std::string prompt = engine_->FormatChatPrompt(user_text, system);

    GenerateParams params;
    int64_t max_tokens = 0;
    if (ReadInt(root, "max_tokens", &max_tokens) ||
        ReadInt(root, "max_completion_tokens", &max_tokens)) {
        params.max_tokens = static_cast<int32_t>(std::clamp<int64_t>(max_tokens, 1, 16384));
    } else {
        params.max_tokens = config_.default_max_tokens;
    }
    double f = 0.0;
    if (ReadDouble(root, "temperature", &f)) {
        params.temperature = static_cast<float>(f);
    }
    if (ReadDouble(root, "top_p", &f)) {
        params.top_p = static_cast<float>(f);
    }
    int64_t top_k = 0;
    if (ReadInt(root, "top_k", &top_k)) {
        params.top_k = static_cast<int32_t>(top_k);
    }
    int64_t seed = 0;
    if (ReadInt(root, "seed", &seed)) {
        params.seed = static_cast<uint64_t>(seed);
    }

    GenerateInput input;
    input.prompt = std::move(prompt);
    input.images = std::move(images);

    std::string text;
    const Status status = Generate(input, params, &text);
    if (!status.ok()) {
        SendError(cntl, 500, "generation failed: " + status.message, "server_error");
        return;
    }

    const PerfStats perf = engine_->last_perf_stats();
    const std::string id = MakeCompletionId();
    JsonBuilder sb;
    sb.start_object();
    sb.append_key_value("id", id);
    sb.append_comma();
    sb.append_key_value("object", "chat.completion");
    sb.append_comma();
    sb.append_key_value("created", NowUnixSeconds());
    sb.append_comma();
    sb.append_key_value("model", config_.model_name);
    sb.append_comma();
    sb.escape_and_append_with_quotes("choices");
    sb.append_colon();
    sb.start_array();
    sb.start_object();
    sb.append_key_value("index", 0);
    sb.append_comma();
    sb.escape_and_append_with_quotes("message");
    sb.append_colon();
    sb.start_object();
    sb.append_key_value("role", "assistant");
    sb.append_comma();
    sb.append_key_value("content", text);
    sb.end_object();
    sb.append_comma();
    sb.append_key_value("finish_reason", "stop");
    sb.end_object();
    sb.end_array();
    sb.append_comma();
    AppendUsage(&sb, perf);
    sb.end_object();
    SendBuiltJson(cntl, 200, &sb);
}

} // namespace pl::mllm::server
