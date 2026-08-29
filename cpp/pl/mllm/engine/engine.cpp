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

#include "cpp/pl/mllm/engine/engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#if defined(__APPLE__)
#include "cpp/pl/mllm/backend/metal/metal_backend.h"
#endif
#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/loader/gguf.h"
#include "cpp/pl/mllm/model/llama_model.h"
#include "cpp/pl/mllm/sampler/sampler.h"
#include "cpp/pl/mllm/tokenizer/tokenizer.h"

namespace pl::mllm {

// Pimpl: holds all engine-owned resources

struct Engine::Impl {
    std::shared_ptr<GGUFFile> gguf;
    ModelConfig config;
    Tokenizer tokenizer;
    std::unique_ptr<LlamaModel> model;
    std::unique_ptr<Backend> backend;
    std::unique_ptr<KVCache> cache;
    std::unique_ptr<ScratchArena> arena;

    // Embedding lookup buffer (f32 for CPU backend).
    TensorView token_embd; // non-owning view into GGUF mmap
    std::vector<LlamaModel::WeightEntry> weight_entries;
};

// Destructor must be in .cpp where Impl is complete.
Engine::~Engine() = default;

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Q8_0 block layout (ggml-compatible, little-endian).
struct Q8Block {
    uint16_t scale; // fp16
    int8_t qs[32];
};
static_assert(sizeof(Q8Block) == kQ8_0TypeSize);

// Copy the embedding row for `token` (row index of token_embd.weight, shape
// [vocab, hidden]) into `out` as f32. Handles f32/f16/Q8_0 embeddings;
// quantized rows are dequantized on the fly (a quantized tensor cannot be
// sliced at arbitrary offsets).
Status embedding_row(const TensorView& embd, int32_t token, int32_t hidden, TensorView out) {
    if (!out.valid() || out.dtype() != DType::kF32 || out.shape().numel() != hidden) {
        return Status::Error(ErrorCode::kInvalidArgument, "embedding: bad output");
    }
    float* od = out.data_as<float>();
    if (embd.dtype() == DType::kF32) {
        const auto* src = embd.data_as<const float>() + static_cast<size_t>(token) * hidden;
        std::memcpy(od, src, static_cast<size_t>(hidden) * sizeof(float));
        return {};
    }
    if (embd.dtype() == DType::kF16) {
        const auto* src = embd.data_as<const uint16_t>() + static_cast<size_t>(token) * hidden;
        for (int32_t i = 0; i < hidden; ++i) {
            od[i] = fp16_to_fp32(src[i]);
        }
        return {};
    }
    if (embd.dtype() == DType::kQ8_0) {
        const int64_t blocks_per_row = hidden / kQ8_0BlockSize;
        const auto* blocks = static_cast<const Q8Block*>(embd.data()) +
                             static_cast<size_t>(token) * static_cast<size_t>(blocks_per_row);
        for (int32_t i = 0; i < hidden; ++i) {
            const int64_t blk = i / kQ8_0BlockSize;
            const int64_t in = i % kQ8_0BlockSize;
            od[i] = fp16_to_fp32(blocks[blk].scale) * static_cast<float>(blocks[blk].qs[in]);
        }
        return {};
    }
    return Status::Error(ErrorCode::kUnsupported, "embedding: unsupported dtype");
}

} // namespace

Result<std::unique_ptr<Engine>> Engine::Create(Options options) {
    auto gguf_result = GGUFFile::Open(options.model_path);
    if (!gguf_result.ok()) {
        return gguf_result.status();
    }
    auto gguf = std::move(gguf_result).value();

    auto cfg_result = gguf->model_config();
    if (!cfg_result.ok()) {
        return cfg_result.status();
    }
    auto config = cfg_result.value();

    auto tok_result = Tokenizer::FromGGUF(*gguf);
    if (!tok_result.ok()) {
        return tok_result.status();
    }
    auto tokenizer = std::move(tok_result).value();

    // Collect weight entries from GGUF tensors.
    std::vector<LlamaModel::WeightEntry> weight_entries;
    for (const auto& ti : gguf->tensors()) {
        auto view_result = gguf->tensor(ti.name);
        if (!view_result.ok()) {
            continue;
        }
        weight_entries.push_back({ti.name, view_result.value()});
    }

    auto model_result = LlamaModel::Create(config, weight_entries);
    if (!model_result.ok()) {
        return model_result.status();
    }
    auto model = std::move(model_result).value();

    // Backend: CPU reference everywhere; Metal GPU only on macOS.
    std::unique_ptr<Backend> backend;
#if defined(__APPLE__)
    if (options.backend == BackendKind::kMetal) {
        backend = std::make_unique<MetalBackend>();
    } else {
        backend = std::make_unique<CpuBackend>();
    }
#else
    if (options.backend == BackendKind::kMetal) {
        return Status::Error(ErrorCode::kUnsupported, "Metal backend requires macOS");
    }
    backend = std::make_unique<CpuBackend>();
#endif
    {
        std::vector<TensorView> views;
        std::vector<std::string_view> names;
        for (const auto& e : weight_entries) {
            views.push_back(e.view);
            names.push_back(e.name);
        }
        if (auto s = backend->ImportWeights(views, names); !s.ok()) {
            return s;
        }
    }

    int32_t max_tokens = std::min(options.max_context, config.context_length);
    auto cache_result = KVCache::Create(config, max_tokens, DType::kF32);
    if (!cache_result.ok()) {
        return cache_result.status();
    }
    auto cache = std::make_unique<KVCache>(std::move(cache_result).value());

    // Arena: generous scratch for per-token activations.
    const size_t arena_bytes = static_cast<size_t>(config.intermediate_size) * 8 * 64;
    auto arena_result = ScratchArena::Create(arena_bytes);
    if (!arena_result.ok()) {
        return arena_result.status();
    }
    auto arena = std::make_unique<ScratchArena>(std::move(arena_result).value());

    // Token embedding view (for lookup).
    auto embd_result = gguf->tensor("token_embd.weight");
    if (!embd_result.ok()) {
        return embd_result.status();
    }

    auto engine = std::unique_ptr<Engine>(new Engine());
    engine->impl_ = std::make_unique<Impl>();
    engine->impl_->gguf = std::move(gguf);
    engine->impl_->config = config;
    engine->impl_->tokenizer = std::move(tokenizer);
    engine->impl_->model = std::move(model);
    engine->impl_->backend = std::move(backend);
    engine->impl_->cache = std::move(cache);
    engine->impl_->arena = std::move(arena);
    engine->impl_->token_embd = embd_result.value();
    engine->impl_->weight_entries = std::move(weight_entries);

    return engine;
}

Status Engine::RunPrefill(std::span<const int32_t> tokens) {
    auto& impl = *impl_;
    const int32_t hidden = impl.config.hidden_size;

    for (int64_t i = 0; i < static_cast<int64_t>(tokens.size()); ++i) {
        int32_t tok = tokens[static_cast<size_t>(i)];
        if (tok < 0 || tok >= impl.config.vocab_size) {
            return Status::Error(ErrorCode::kInvalidArgument, "prefill: token out of vocab range");
        }

        // Embedding lookup: dequantize the row into a writable arena buffer
        // (Forward modifies hidden in-place). Handles f32/f16/Q8_0 weights.
        impl.arena->Reset();
        auto embd_buf = impl.arena->AllocateTensor({1, hidden}, DType::kF32);
        if (!embd_buf.ok())
            return embd_buf.status();
        if (auto s = embedding_row(impl.token_embd, tok, hidden, embd_buf.value()); !s.ok()) {
            return s;
        }

        if (auto s =
                impl.model->Forward(embd_buf.value(), i, *impl.cache, *impl.backend, *impl.arena);
            !s.ok()) {
            return s;
        }
    }
    return {};
}

Result<std::vector<int32_t>> Engine::GenerateTokens(std::string_view prompt,
                                                    GenerateParams params) {
    std::vector<int32_t> result;
    auto s = GenerateStream(prompt, params, [&](std::string_view, int32_t tok) {
        result.push_back(tok);
        return true;
    });
    if (!s.ok() && s.code != ErrorCode::kCancelled) {
        return s;
    }
    return result;
}

Status Engine::GenerateStream(std::string_view prompt,
                              GenerateParams params,
                              std::function<bool(std::string_view, int32_t)> on_piece) {
    auto& impl = *impl_;
    const int32_t hidden = impl.config.hidden_size;
    const int32_t vocab = impl.config.vocab_size;

    // Tokenize prompt.
    auto enc_result = impl.tokenizer.Encode(prompt, true);
    if (!enc_result.ok()) {
        return enc_result.status();
    }
    auto prompt_tokens = enc_result.value();

    if (static_cast<int32_t>(prompt_tokens.size()) + params.max_tokens > impl.cache->capacity()) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "prompt + max_tokens exceeds cache capacity");
    }

    auto t_start = Clock::now();

    // Prefill.
    auto prefill_start = Clock::now();
    if (auto s = RunPrefill(prompt_tokens); !s.ok()) {
        return s;
    }
    auto prefill_end = Clock::now();
    perf_.prefill_ms = elapsed_ms(prefill_start, prefill_end);
    perf_.prompt_tokens = static_cast<int32_t>(prompt_tokens.size());

    // Set up sampler.
    SamplerParams sp;
    sp.temperature = params.temperature;
    sp.top_k = params.top_k;
    sp.top_p = params.top_p;
    sp.repeat_penalty = params.repeat_penalty;
    sp.seed = params.seed;
    Sampler sampler(sp);

    // Logits buffer.
    auto logits_buf_result = OwnedBuffer::AllocateCpu(static_cast<size_t>(vocab) * 4, 64);
    if (!logits_buf_result.ok()) {
        return logits_buf_result.status();
    }
    auto logits_owned = std::move(logits_buf_result).value();
    TensorView logits(logits_owned.data(), DType::kF32, {1, vocab});

    // Decode loop.
    std::vector<int32_t> generated;
    generated.reserve(static_cast<size_t>(params.max_tokens));
    int64_t pos = static_cast<int64_t>(prompt_tokens.size());

    auto decode_start = Clock::now();
    bool first_token = true;

    // The last token from prompt seeds the first decode step.
    int32_t last_token = prompt_tokens.back();

    for (int32_t step = 0; step < params.max_tokens; ++step) {
        // Embed last token into a writable arena buffer (dequantized).
        if (last_token < 0 || last_token >= vocab) {
            return Status::Error(ErrorCode::kInternal, "token out of range");
        }

        impl.arena->Reset();
        auto embd_buf = impl.arena->AllocateTensor({1, hidden}, DType::kF32);
        if (!embd_buf.ok())
            return embd_buf.status();
        if (auto s = embedding_row(impl.token_embd, last_token, hidden, embd_buf.value());
            !s.ok()) {
            return s;
        }

        if (auto s =
                impl.model->Forward(embd_buf.value(), pos, *impl.cache, *impl.backend, *impl.arena);
            !s.ok()) {
            return s;
        }

        impl.arena->Reset();
        if (auto s =
                impl.model->ComputeLogits(embd_buf.value(), logits, *impl.backend, *impl.arena);
            !s.ok()) {
            return s;
        }

        // Sample.
        sp.penalty_tokens = generated;
        int32_t next = sampler.Sample(logits.span_as<float>());

        if (first_token) {
            perf_.time_to_first_token_ms = elapsed_ms(t_start, Clock::now());
            first_token = false;
        }

        // EOS check.
        if (next == impl.tokenizer.eos_id()) {
            break;
        }

        generated.push_back(next);

        // Decode and stream.
        auto text_result = impl.tokenizer.DecodeOne(next);
        if (text_result.ok()) {
            if (!on_piece(text_result.value(), next)) {
                return Status::Error(ErrorCode::kCancelled, "cancelled by callback");
            }
        }

        last_token = next;
        ++pos;
    }

    auto decode_end = Clock::now();
    perf_.decode_ms = elapsed_ms(decode_start, decode_end);
    perf_.generated_tokens = static_cast<int32_t>(generated.size());
    perf_.total_ms = elapsed_ms(t_start, decode_end);
    if (perf_.generated_tokens > 0 && perf_.decode_ms > 0) {
        perf_.tok_per_sec = perf_.generated_tokens / (perf_.decode_ms / 1000.0);
    }

    return {};
}

} // namespace pl::mllm
