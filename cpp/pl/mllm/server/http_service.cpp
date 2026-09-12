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

#include <array>
#include <atomic>
#include <brpc/progressive_attachment.h>
#include <butil/logging.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <simdjson.h>
#include <string>
#include <utility>
#include <vector>

#include "cpp/pl/mllm/media/image_io.h"
#include "cpp/pl/mllm/server/http_util.h"

namespace pl::mllm::server {

namespace {

// ---------------------------------------------------------------------------
// JSON builders — simdjson::builder::string_builder handles escaping and
// number formatting; `builtin` is the best statically-available
// implementation (public, stable alias).
// ---------------------------------------------------------------------------

using JsonBuilder = simdjson::builtin::builder::string_builder;

void SendJson(brpc::Controller* cntl,
              int status,
              std::string body,
              brpc::ProgressiveAttachment* pa = nullptr) {
    auto& res = cntl->http_response();
    res.set_status_code(status);
    res.set_content_type("application/json; charset=utf-8");
    // Permissive CORS: the OpenAI surface is meant for local tooling and
    // browser UIs alike.
    res.SetHeader("Access-Control-Allow-Origin", "*");
    if (pa != nullptr) {
        // A created ProgressiveAttachment owns the response body (writes made
        // before done are buffered and flushed after the headers).
        // response_attachment() is IGNORED once one exists.
        if (pa->Write(body.data(), body.size()) != 0) {
            LOG(WARNING) << "progressive write failed (client gone?), errno=" << errno;
        }
    } else {
        cntl->response_attachment().append(std::move(body));
    }
}

// Serializes the builder content and sends it. The builder's only failure
// mode is buffer allocation, mapped to a 500.
void SendBuiltJson(brpc::Controller* cntl,
                   int status,
                   JsonBuilder* sb,
                   brpc::ProgressiveAttachment* pa = nullptr) {
    std::string_view sv;
    if (sb->view().get(sv) != simdjson::SUCCESS) {
        SendJson(
            cntl,
            500,
            R"({"error":{"message":"json buffer allocation failed","type":"server_error","code":null}})",
            pa);
        return;
    }
    SendJson(cntl, status, std::string(sv), pa);
}

// OpenAI-style error body: {"error":{"message":...,"type":...,"code":null}}.
void SendError(brpc::Controller* cntl,
               int status,
               std::string_view message,
               std::string_view type = "invalid_request_error",
               brpc::ProgressiveAttachment* pa = nullptr) {
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
    SendBuiltJson(cntl, status, &sb, pa);
}

// Maps a kernel-side Status to (HTTP status, OpenAI error type).
void SendStatusError(brpc::Controller* cntl, const Status& status) {
    switch (status.code) {
        case ErrorCode::kInvalidArgument:
            SendError(cntl, 400, status.message);
            return;
        case ErrorCode::kUnsupported:
            SendError(cntl, 400, status.message, "unsupported_parameter");
            return;
        default:
            SendError(cntl, 500, status.message, "server_error");
            return;
    }
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

// Self-deleting Closure wrapping a std::function (this repo's protobuf
// NewCallback only binds plain functions/methods, not lambdas).
class FunctionClosure : public google::protobuf::Closure {
public:
    explicit FunctionClosure(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Run() override {
        fn_();
        delete this;
    }

private:
    std::function<void()> fn_;
};

// Sets up client-disconnect detection for a generation request: when the
// connection dies, `cancel` is flipped and the engine's streaming callback
// aborts on its next piece. The callback always runs (also on attachment
// destruction); it is harmless after the request is done.
//
// Returns null when progressive attachments are unavailable (non-HTTP
// protocol); caller then serves without cancellation. Note: once an
// attachment exists, ALL response bodies must be written through it.
butil::intrusive_ptr<brpc::ProgressiveAttachment> WatchClientClose(
    brpc::Controller* cntl, const std::shared_ptr<std::atomic_bool>& cancel) {
    butil::intrusive_ptr<brpc::ProgressiveAttachment> pa = cntl->CreateProgressiveAttachment();
    if (pa == nullptr) {
        LOG(WARNING) << "no progressive attachment (non-HTTP?); "
                        "client-disconnect cancellation disabled";
        return nullptr;
    }
    pa->NotifyOnStopped(
        new FunctionClosure([cancel] { cancel->store(true, std::memory_order_relaxed); }));
    return pa;
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

    if (method == brpc::HTTP_METHOD_OPTIONS) {
        HandleCorsPreflight(cntl);
        return;
    }
    // Body size guardrail: reject before paying the parse cost.
    if (method == brpc::HTTP_METHOD_POST &&
        cntl->request_attachment().size() > static_cast<size_t>(config_.max_body_bytes)) {
        SendError(cntl, 413, "request body too large");
        return;
    }

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

void MllmHttpService::HandleCorsPreflight(brpc::Controller* cntl) {
    auto& res = cntl->http_response();
    res.set_status_code(204);
    res.SetHeader("Access-Control-Allow-Origin", "*");
    res.SetHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.SetHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    res.SetHeader("Access-Control-Max-Age", "86400");
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
                                 const std::shared_ptr<std::atomic_bool>& cancel,
                                 std::string* out,
                                 PerfStats* out_stats) {
    std::lock_guard<bthread::Mutex> lock(engine_mu_);
    out->clear();
    Status status =
        engine_->GenerateStream(input, params, [&cancel, out](std::string_view piece, int32_t) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                return false;
            }
            out->append(piece);
            return true;
        });
    // The engine-owned PerfStats is overwritten by the next generation, so it
    // must be checkpointed while the lock is still held.
    if (out_stats != nullptr) {
        *out_stats = engine_->last_perf_stats();
    }
    return status;
}

void MllmHttpService::HandleOcr(brpc::Controller* cntl) {
    if (!engine_->has_vision()) {
        SendError(cntl, 503, "engine has no vision tower (start with --mmproj)", "server_error");
        return;
    }

    const std::string body_str = cntl->request_attachment().to_string();
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(body_str).get(root) != simdjson::SUCCESS) {
        SendError(cntl, 400, "request body is not valid JSON");
        return;
    }

    auto parsed = ParseOcrRequest(root, config_);
    if (!parsed.ok()) {
        SendStatusError(cntl, parsed.status());
        return;
    }

    std::vector<uint8_t> image_bytes;
    if (!DecodeImagePayload(parsed.value().image_payload, &image_bytes) || image_bytes.empty()) {
        SendError(cntl, 400, "invalid base64 image payload");
        return;
    }
    if (static_cast<int64_t>(image_bytes.size()) > config_.max_image_bytes) {
        SendError(cntl, 400, "decoded image exceeds the size limit");
        return;
    }
    auto image = media::LoadImageData(image_bytes.data(), image_bytes.size());
    if (!image.ok()) {
        SendError(cntl, 400, "cannot decode image: " + image.status().message);
        return;
    }

    // Expand the OCR prompt scaffold: {IMAGE} -> the START/placeholder/END
    // triple (llama.cpp mtmd layout), {TASK} -> the request's task string.
    const std::string image_slot = "<|IMAGE_START|>" + config_.image_placeholder + "<|IMAGE_END|>";
    std::string prompt = ExpandOcrTemplate(config_.ocr_template, image_slot, parsed.value().task);

    GenerateParams params;
    params.max_tokens = parsed.value().max_tokens;

    GenerateInput input;
    input.prompt = std::move(prompt);
    input.images.push_back(std::move(image).value());

    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto pa = WatchClientClose(cntl, cancel);

    std::string text;
    PerfStats perf;
    const Status status = Generate(input, params, cancel, &text, &perf);
    if (cancel->load(std::memory_order_relaxed)) {
        // Client is gone; no one will read the response.
        LOG(INFO) << "/v1/ocr: client disconnected, generation "
                  << (status.ok() ? "completed" : "cancelled") << " and response dropped";
        return;
    }
    if (!status.ok()) {
        SendError(cntl,
                  status.code == ErrorCode::kCancelled ? 499 : 500,
                  "generation failed: " + status.message,
                  "server_error",
                  pa.get());
        return;
    }

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
    SendBuiltJson(cntl, 200, &sb, pa.get());
}

void MllmHttpService::HandleChatCompletions(brpc::Controller* cntl) {
    const std::string body_str = cntl->request_attachment().to_string();
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(body_str).get(root) != simdjson::SUCCESS) {
        SendError(cntl, 400, "request body is not valid JSON");
        return;
    }

    auto parsed = ParseChatRequest(root, config_);
    if (!parsed.ok()) {
        SendStatusError(cntl, parsed.status());
        return;
    }

    // Vision capability check before any decode work: on a text-only engine
    // an image request fails fast here instead of after base64 + image decode.
    if (!parsed.value().image_payloads.empty() && !engine_->has_vision()) {
        SendError(cntl, 503, "engine has no vision tower (start with --mmproj)", "server_error");
        return;
    }

    std::vector<media::Image> images;
    images.reserve(parsed.value().image_payloads.size());
    for (const std::string& payload : parsed.value().image_payloads) {
        std::vector<uint8_t> bytes;
        if (!DecodeImagePayload(payload, &bytes) || bytes.empty()) {
            SendError(cntl, 400, "invalid image_url payload (expecting a data: URL)");
            return;
        }
        if (static_cast<int64_t>(bytes.size()) > config_.max_image_bytes) {
            SendError(cntl, 400, "decoded image exceeds the size limit");
            return;
        }
        auto img = media::LoadImageData(bytes.data(), bytes.size());
        if (!img.ok()) {
            SendError(cntl, 400, "cannot decode image: " + img.status().message);
            return;
        }
        images.push_back(std::move(img).value());
    }

    // Splice images into the user turn before templating, so the visual
    // tokens land inside the model's trained "User: <image>..." layout.
    std::string user_text = std::move(parsed.value().user_text);
    if (!images.empty() && user_text.find(config_.image_placeholder) == std::string::npos) {
        std::string prefixed;
        prefixed.reserve(images.size() * (config_.image_placeholder.size() + 32) +
                         user_text.size());
        for (size_t i = 0; i < images.size(); ++i) {
            prefixed += "<|IMAGE_START|>" + config_.image_placeholder + "<|IMAGE_END|>";
        }
        user_text = prefixed + user_text;
    }
    std::string prompt = engine_->FormatChatPrompt(user_text, parsed.value().system);

    GenerateParams params = parsed.value().params;
    GenerateInput input;
    input.prompt = std::move(prompt);
    input.images = std::move(images);

    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto pa = WatchClientClose(cntl, cancel);

    std::string text;
    PerfStats perf;
    const Status status = Generate(input, params, cancel, &text, &perf);
    if (cancel->load(std::memory_order_relaxed)) {
        LOG(INFO) << "/v1/chat/completions: client disconnected, generation "
                  << (status.ok() ? "completed" : "cancelled") << " and response dropped";
        return;
    }
    if (!status.ok()) {
        SendError(cntl,
                  status.code == ErrorCode::kCancelled ? 499 : 500,
                  "generation failed: " + status.message,
                  "server_error",
                  pa.get());
        return;
    }

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
    // OpenAI semantics: "length" iff the token cap was hit, else "stop".
    sb.append_key_value("finish_reason", FinishReasonFor(perf, params.max_tokens));
    sb.end_object();
    sb.end_array();
    sb.append_comma();
    AppendUsage(&sb, perf);
    sb.end_object();
    SendBuiltJson(cntl, 200, &sb, pa.get());
}

} // namespace pl::mllm::server
