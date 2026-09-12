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

#include <atomic>
#include <brpc/controller.h>
#include <bthread/mutex.h>
#include <cstdint>
#include <memory>
#include <string>

#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/server/mllm.pb.h"
#include "cpp/pl/mllm/server/server_config.h"

namespace pl::mllm::server {

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
//                                   non-streaming, single-turn only.
//   OPTIONS *                    -> CORS preflight
//
// Concurrency model:
// - The engine is not thread-safe (and the GPU wouldn't benefit anyway), so
//   all generations serialize on `engine_mu_`. It is a bthread::Mutex (NOT
//   std::mutex): queued requests merely suspend their bthread, keeping the
//   underlying brpc worker threads free for /healthz and new connections.
// - PerfStats are copied out while still holding the lock; the engine-owned
//   copy is overwritten by the next generation, so reading it after the
//   lock is released would race (and could return another request's stats).
// - Both generation endpoints watch the client connection (via a
//   ProgressiveAttachment's NotifyOnStopped): a disconnecting client flips
//   an atomic flag that the streaming callback polls, cancelling generation
//   instead of burning GPU on a response nobody will read.
class MllmHttpService : public proto::MllmHttpService {
public:
    MllmHttpService(Engine* engine, ServerConfig config);

    void default_method(google::protobuf::RpcController* controller,
                        const proto::HttpRequest* /*request*/,
                        proto::HttpResponse* /*response*/,
                        google::protobuf::Closure* done) override;

private:
    void HandleHealthz(brpc::Controller* cntl);
    void HandleCorsPreflight(brpc::Controller* cntl);
    void HandleListModels(brpc::Controller* cntl);
    void HandleOcr(brpc::Controller* cntl);
    void HandleChatCompletions(brpc::Controller* cntl);

    // Runs one generation under the engine mutex, collecting all streamed
    // pieces into `out`. The callback polls `cancel` (set by the
    // client-disconnect watcher) and aborts when flipped. `out_stats` is
    // checkpointed from the engine while the lock is still held.
    Status Generate(const GenerateInput& input,
                    GenerateParams params,
                    const std::shared_ptr<std::atomic_bool>& cancel,
                    std::string* out,
                    PerfStats* out_stats);

    Engine* engine_; // not owned; owned by main()
    ServerConfig config_;
    bthread::Mutex engine_mu_;
};

} // namespace pl::mllm::server
