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
#include "cpp/pl/mllm/model/model.h"
#include "cpp/pl/mllm/sampler/sampler.h"
#include "cpp/pl/mllm/tokenizer/tokenizer.h"

namespace pl::mllm {

// Pimpl: holds all engine-owned resources

struct Engine::Impl {
    std::shared_ptr<GGUFFile> gguf;
    ModelConfig config;
    Tokenizer tokenizer;
    std::unique_ptr<Model> model;
    std::unique_ptr<Backend> backend;
    std::unique_ptr<KVCache> cache;
    std::unique_ptr<ScratchArena> arena;

    // Embedding lookup buffer (f32 for CPU backend).
    TensorView token_embd; // non-owning view into GGUF mmap
    std::vector<Model::WeightEntry> weight_entries;

    // Persistent hidden-state buffer for batched prefill:
    // [kPrefillChunk, hidden_size] f32. Kept engine-owned (not in the scratch
    // arena) because the residual stream must survive per-layer arena resets,
    // and RunPrefill hands a view of the last row to the sampler.
    OwnedBuffer prefill_hidden;

    // Raw `tokenizer.chat_template` from GGUF metadata (jinja source).
    // Empty = model ships no template.
    std::string chat_template;
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
    std::vector<Model::WeightEntry> weight_entries;
    for (const auto& ti : gguf->tensors()) {
        auto view_result = gguf->tensor(ti.name);
        if (!view_result.ok()) {
            continue;
        }
        weight_entries.push_back({ti.name, view_result.value()});
    }

    auto model_result = CreateModel(config, weight_entries);
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
    std::unique_ptr<KVCache> cache;
    if (backend->HasDeviceKV()) {
        // The backend owns the real K/V storage on device; the host cache is
        // a metadata-only shell for length/capacity bookkeeping.
        if (auto s = backend->ConfigureDeviceKV(
                config.num_layers, config.num_kv_heads, config.effective_head_dim(), max_tokens);
            !s.ok()) {
            return s;
        }
        auto shell_result = KVCache::CreateShell(config, max_tokens);
        if (!shell_result.ok()) {
            return shell_result.status();
        }
        cache = std::make_unique<KVCache>(std::move(shell_result).value());
    } else {
        auto cache_result = KVCache::Create(config, max_tokens, DType::kF32);
        if (!cache_result.ok()) {
            return cache_result.status();
        }
        cache = std::make_unique<KVCache>(std::move(cache_result).value());
    }

    // Arena sizing: the decode path accumulates activations of ALL layers in
    // one forward pass (arena reset per token), so it needs ~per_layer *
    // num_layers * 2. The batched-prefill path resets the arena per layer and
    // needs ~per_layer * kPrefillChunk * 2 (chunk rows instead of one).
    // Allocate the max of both, with headroom for logits.
    const size_t per_layer_bytes =
        static_cast<size_t>(std::max(config.intermediate_size,
                                     config.num_attention_heads * config.effective_head_dim())) *
        10 * 4;
    const size_t arena_bytes =
        std::max(per_layer_bytes * static_cast<size_t>(config.num_layers),
                 per_layer_bytes * static_cast<size_t>(kPrefillChunk)) *
            2 +
        65536;
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

    // Chat template (jinja source) — empty when the model ships none.
    std::string chat_template;
    if (auto tmpl = gguf->string_meta("tokenizer.chat_template"); tmpl.ok()) {
        chat_template = std::move(tmpl).value();
    }

    auto engine = std::unique_ptr<Engine>(new Engine());
    engine->impl_ = std::make_unique<Impl>();
    auto prefill_buf =
        OwnedBuffer::AllocateCpu(static_cast<size_t>(engine->kPrefillChunk) *
                                     static_cast<size_t>(config.hidden_size) * sizeof(float),
                                 64);
    if (!prefill_buf.ok()) {
        return prefill_buf.status();
    }
    engine->impl_->prefill_hidden = std::move(prefill_buf).value();
    engine->impl_->gguf = std::move(gguf);
    engine->impl_->config = config;
    engine->impl_->tokenizer = std::move(tokenizer);
    engine->impl_->model = std::move(model);
    engine->impl_->backend = std::move(backend);
    engine->impl_->cache = std::move(cache);
    engine->impl_->arena = std::move(arena);
    engine->impl_->token_embd = embd_result.value();
    engine->impl_->weight_entries = std::move(weight_entries);
    engine->impl_->chat_template = std::move(chat_template);

    return engine;
}

Result<TensorView> Engine::RunPrefill(std::span<const int32_t> tokens) {
    auto& impl = *impl_;
    const int32_t hidden = impl.config.hidden_size;

    if (tokens.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "prefill: empty prompt");
    }

    // Batched prefill: process the prompt in chunks of kPrefillChunk tokens.
    // All GEMMs/norms/activations run batch-wide; attention enforces causal
    // masking by giving query row i exactly the [0, chunk_start + i] prefix.
    auto* dst = static_cast<float*>(impl.prefill_hidden.data());
    int64_t row_of_last = 0;
    for (int64_t chunk_start = 0; chunk_start < static_cast<int64_t>(tokens.size());
         chunk_start += kPrefillChunk) {
        const int32_t n = static_cast<int32_t>(
            std::min<int64_t>(kPrefillChunk,
                              static_cast<int64_t>(tokens.size()) - chunk_start));

        // Embed the chunk's tokens into rows of the persistent prefill
        // buffer (Forward modifies them in-place). Handles f32/f16/Q8_0.
        for (int32_t i = 0; i < n; ++i) {
            const int32_t tok = tokens[static_cast<size_t>(chunk_start + i)];
            if (tok < 0 || tok >= impl.config.vocab_size) {
                return Status::Error(ErrorCode::kInvalidArgument,
                                     "prefill: token out of vocab range");
            }
            TensorView row(dst + static_cast<size_t>(i) * hidden,
                           DType::kF32,
                           Shape({1, hidden}));
            if (auto s = embedding_row(impl.token_embd, tok, hidden, row); !s.ok()) {
                return s;
            }
        }
        TensorView batch(dst, DType::kF32, Shape({n, hidden}));
        // The embedding writes happened on the host, outside the backend.
        if (auto s = impl.backend->NotifyHostWrite(batch); !s.ok()) {
            return s;
        }

        if (auto s = impl.model->Prefill(
                batch, chunk_start, *impl.cache, *impl.backend, *impl.arena);
            !s.ok()) {
            return s;
        }
        // Bring the transformed chunk back to host memory. The caller holds
        // only row VIEWs of prefill_hidden afterwards; backends with device
        // residency would otherwise serve stale host data when the logits
        // path uploads the last row (the row pointer has no shadow entry).
        if (auto s = impl.backend->SyncToHost(batch); !s.ok()) {
            return s;
        }
        row_of_last = n - 1;
    }

    // Return the final hidden state (last row of the last chunk) so the
    // caller can sample the first generated token directly.
    return TensorView(dst + row_of_last * static_cast<size_t>(hidden),
                      DType::kF32,
                      Shape({1, hidden}));
}

bool Engine::has_chat_template() const noexcept {
    return !impl_->chat_template.empty();
}

// Minimal chat-template renderer. Full jinja is out of scope; instead we
// recognize the template FAMILY by its control tokens and emit the
// well-known canonical serialization for a single user turn (this matches
// what llama.cpp's jinja renderer produces for plain conversations).
std::string Engine::FormatChatPrompt(std::string_view user, std::string_view system) const {
    const std::string& tpl = impl_->chat_template;

    // ChatML family (Qwen, DeepSeek, ...): "<|im_start|>" markers.
    if (tpl.find("<|im_start|>") != std::string::npos) {
        std::string out;
        if (!system.empty()) {
            out += "<|im_start|>system\n";
            out += system;
            out += "<|im_end|>\n";
        }
        out += "<|im_start|>user\n";
        out += user;
        out += "<|im_end|>\n<|im_start|>assistant\n";
        return out;
    }

    // Llama-3 family: "<|start_header_id|>"/"<|eot_id|>" markers.
    if (tpl.find("<|start_header_id|>") != std::string::npos) {
        std::string out = "<|begin_of_text|>";
        if (!system.empty()) {
            out += "<|start_header_id|>system<|end_header_id|>\n\n";
            out += system;
            out += "<|eot_id|>";
        }
        out += "<|start_header_id|>user<|end_header_id|>\n\n";
        out += user;
        out += "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";
        return out;
    }

    // Llama-2 family: "[INST]" markers. System goes inside the first
    // instruction, per the reference template.
    if (tpl.find("[INST]") != std::string::npos) {
        std::string out = "[INST] ";
        if (!system.empty()) {
            out += "<<SYS>>\n";
            out += system;
            out += "\n<</SYS>>\n\n";
        }
        out += user;
        out += " [/INST]";
        return out;
    }

    // No recognized template: pass the user message through verbatim.
    return std::string(user);
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

    // Prefill. The returned hidden state belongs to the last prompt token
    // and is used directly to sample the first generated token — the last
    // prompt token must NOT be re-forwarded during decode.
    auto prefill_start = Clock::now();
    auto prefill_result = RunPrefill(prompt_tokens);
    if (!prefill_result.ok()) {
        return prefill_result.status();
    }
    TensorView hidden_state = prefill_result.value();
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

    // Decode loop: each step samples from the current hidden state, then
    // embeds the sampled token and forwards it to produce the next one.
    std::vector<int32_t> generated;
    generated.reserve(static_cast<size_t>(params.max_tokens));
    // Scratch for the repetition-penalty context window (only touched when
    // repeat_penalty is enabled).
    std::vector<int32_t> penalty_ctx;
    int64_t pos = static_cast<int64_t>(prompt_tokens.size());

    auto decode_start = Clock::now();
    bool first_token = true;

    for (int32_t step = 0; step < params.max_tokens; ++step) {
        // Compute logits from the current hidden state. The arena is NOT
        // reset between Forward and ComputeLogits: `hidden_state` lives in
        // the arena and ComputeLogits' scratch allocations follow it.
        if (auto s = impl.model->ComputeLogits(hidden_state, logits, *impl.backend, *impl.arena);
            !s.ok()) {
            return s;
        }

        // The sampler reads logits on the host.
        if (auto s = impl.backend->SyncToHost(logits); !s.ok()) {
            return s;
        }

        // Sample. The repetition penalty applies to the recent context:
        // prompt tail + generated tokens, capped at a 64-token window
        // (llama.cpp's penalty_last_n default).
        if (params.repeat_penalty != 1.0f) {
            constexpr size_t kPenaltyLastN = 64;
            // Only the last kPenaltyLastN tokens of (prompt + generated)
            // matter; build the window directly instead of copying both
            // full vectors (O(window) instead of O(context) per step).
            const size_t take_gen = std::min(kPenaltyLastN, generated.size());
            size_t take_prompt = prompt_tokens.size();
            if (take_gen + take_prompt > kPenaltyLastN) {
                take_prompt = kPenaltyLastN - take_gen;
            }
            penalty_ctx.assign(prompt_tokens.end() - take_prompt, prompt_tokens.end());
            penalty_ctx.insert(
                penalty_ctx.end(), generated.end() - take_gen, generated.end());
            sampler.set_penalty_tokens(std::span<const int32_t>(penalty_ctx));
        }
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

        // No point forwarding a token nobody will sample from.
        if (step + 1 == params.max_tokens) {
            break;
        }

        // Embed the sampled token into a writable arena buffer (dequantized)
        // and forward it at the next position.
        if (next < 0 || next >= vocab) {
            return Status::Error(ErrorCode::kInternal, "token out of range");
        }

        impl.arena->Reset();
        auto embd_buf = impl.arena->AllocateTensor({1, hidden}, DType::kF32);
        if (!embd_buf.ok())
            return embd_buf.status();
        if (auto s = embedding_row(impl.token_embd, next, hidden, embd_buf.value()); !s.ok()) {
            return s;
        }
        // The embedding write happened on the host, outside the backend.
        if (auto s = impl.backend->NotifyHostWrite(embd_buf.value()); !s.ok()) {
            return s;
        }

        if (auto s =
                impl.model->Forward(embd_buf.value(), pos, *impl.cache, *impl.backend, *impl.arena);
            !s.ok()) {
            return s;
        }
        // Forward updates the buffer in place; it is now the hidden state
        // for the next sampling step.
        hidden_state = embd_buf.value();
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
