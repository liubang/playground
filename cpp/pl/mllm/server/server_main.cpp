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

#include <atomic>
#include <brpc/server.h>
#include <butil/logging.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <gflags/gflags.h>
#include <string>
#include <thread>
#include <utility>

#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/server/http_service.h"
#include "cpp/pl/mllm/server/log_sink.h"

DEFINE_string(model, "", "Path to the LLM GGUF checkpoint (required)");
DEFINE_string(mmproj,
              "",
              "Vision tower weights (mmproj GGUF, e.g. PaddleOCR-VL); empty = text-only server");
DEFINE_string(backend, "cpu", "Inference backend: cpu | metal");
DEFINE_bool(ring, false, "Sliding-window (ring) KV cache (see mllm_cli --ring)");
DEFINE_int32(ctx, 8192, "Max context / KV-cache capacity");
DEFINE_string(image_token, "<|IMAGE_PLACEHOLDER|>", "Vocab piece marking an image slot");
DEFINE_string(mrope_section,
              "",
              "Explicit MRoPE (t,h,w) section split overriding GGUF metadata, e.g. \"24,20,20\"");

DEFINE_string(listen, "127.0.0.1", "Listen address");
DEFINE_int32(port, 8310, "TCP port of this server");
DEFINE_int32(idle_timeout_s,
             -1,
             "Close connections idle for this many seconds; -1 disables it. Keep disabled: "
             "a long generation produces no socket traffic and would trip the timeout");

DEFINE_string(model_name,
              "",
              "Model id reported by /v1/models and echoed in responses; "
              "defaults to the model file's basename without .gguf");
DEFINE_string(ocr_template,
              "<|begin_of_sentence|>User: {IMAGE}{TASK}\nAssistant:\n",
              "Prompt scaffold for /v1/ocr; {IMAGE} and {TASK} are substituted");
DEFINE_string(ocr_task, "OCR:", "Default /v1/ocr task when the request omits \"prompt\"");
DEFINE_int32(default_max_tokens, 2048, "Generation cap when a request omits max_tokens");

DEFINE_string(log_file,
              "",
              "Log file path; empty = log to stderr. When set, logs are written to a "
              "size-capped, generation-rotated file (self-managed, see log_sink.h)");
DEFINE_int32(log_max_size_mb, 10, "Per-file log size cap before rotation");
DEFINE_int32(log_max_files, 3, "Rotated generations kept (<path>.1.log .. .N.log)");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Self-managed rotating log file. The sink is intentionally leaked:
    // butil::SetLogSink holds a raw pointer and logging continues until
    // process death (static destruction would race late loggers).
    if (!FLAGS_log_file.empty()) {
        pl::mllm::server::RotatingLogSink::Options log_opts;
        log_opts.path = FLAGS_log_file;
        log_opts.max_bytes = static_cast<int64_t>(FLAGS_log_max_size_mb) * 1024 * 1024;
        log_opts.max_generations = FLAGS_log_max_files;
        logging::SetLogSink(new pl::mllm::server::RotatingLogSink(std::move(log_opts)));
    }

    if (FLAGS_model.empty()) {
        std::fprintf(stderr, "error: --model is required (see --help)\n");
        return 1;
    }

    pl::mllm::Engine::Options engine_opts;
    engine_opts.model_path = FLAGS_model;
    engine_opts.max_context = FLAGS_ctx;
    engine_opts.backend =
        FLAGS_backend == "metal" ? pl::mllm::BackendKind::kMetal : pl::mllm::BackendKind::kCpu;
    engine_opts.ring = FLAGS_ring;
    engine_opts.mmproj_path = FLAGS_mmproj;
    engine_opts.image_placeholder = FLAGS_image_token;
    if (!FLAGS_mrope_section.empty()) {
        if (std::sscanf(FLAGS_mrope_section.c_str(),
                        "%d,%d,%d",
                        &engine_opts.mrope_section[0],
                        &engine_opts.mrope_section[1],
                        &engine_opts.mrope_section[2]) != 3) {
            std::fprintf(stderr, "error: --mrope_section expects t,h,w\n");
            return 1;
        }
    }

    LOG(INFO) << "loading model " << FLAGS_model << " backend=" << FLAGS_backend
              << (FLAGS_mmproj.empty() ? "" : " mmproj=" + FLAGS_mmproj);
    auto engine_result = pl::mllm::Engine::Create(std::move(engine_opts));
    if (!engine_result.ok()) {
        LOG(ERROR) << "engine init failed: " << engine_result.status().message;
        return 1;
    }
    auto engine = std::move(engine_result).value();

    pl::mllm::server::ServerConfig config;
    if (!FLAGS_model_name.empty()) {
        config.model_name = FLAGS_model_name;
    } else {
        // Default model id: basename of the checkpoint without the .gguf suffix.
        const size_t slash = FLAGS_model.find_last_of('/');
        std::string base = FLAGS_model.substr(slash == std::string::npos ? 0 : slash + 1);
        if (base.ends_with(".gguf")) {
            base.resize(base.size() - 5);
        }
        config.model_name = std::move(base);
    }
    config.image_placeholder = FLAGS_image_token;
    config.ocr_template = FLAGS_ocr_template;
    config.ocr_task = FLAGS_ocr_task;
    config.default_max_tokens = FLAGS_default_max_tokens;

    // Kept for the startup log below; `config` itself is moved into the service.
    const std::string model_name = config.model_name;
    pl::mllm::server::MllmHttpService service(engine.get(), std::move(config));

    brpc::Server server;
    if (server.AddService(&service,
                          brpc::SERVER_DOESNT_OWN_SERVICE,
                          "/v1/* => default_method,"
                          "/healthz => default_method") != 0) {
        LOG(ERROR) << "failed to add MllmHttpService";
        return 1;
    }

    // brpc installs no signal handlers by default; handle SIGINT/SIGTERM
    // ourselves for a graceful shutdown (same as minisearch_server).
    // Must be installed before server.Start.
    static std::atomic<bool> quit{false};
    std::signal(SIGINT, [](int) { quit.store(true); });
    std::signal(SIGTERM, [](int) { quit.store(true); });

    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;

    const std::string address = FLAGS_listen + ":" + std::to_string(FLAGS_port);
    if (server.Start(address.c_str(), &options) != 0) {
        LOG(ERROR) << "failed to start server on " << address;
        return 1;
    }

    LOG(INFO) << "mllm server (HTTP/JSON) on " << address << " model=" << model_name
              << " vision=" << (engine->has_vision() ? "on" : "off") << " ctx=" << FLAGS_ctx
              << " endpoints: /healthz /v1/models /v1/ocr /v1/chat/completions";

    while (!quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG(INFO) << "shutting down...";
    server.Stop(0);
    server.Join();
    return 0;
}
