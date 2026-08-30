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
// Created: 2026/08/30

// Diagnostic tool: load a real GGUF model, prefill a short prompt and dump
// configuration, per-layer activation stats, logit statistics and top-k
// tokens, plus a few greedy decode steps. Useful to validate model loading
// and numerical health against real checkpoints.
//
//   bazel run //cpp/pl/mllm/tools:dump_logits -- -m <model.gguf> -p <prompt>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/loader/gguf.h"
#include "cpp/pl/mllm/model/model.h"
#include "cpp/pl/mllm/model/transformer_layer.h"
#include "cpp/pl/mllm/model/weight_names.h"
#include "cpp/pl/mllm/tokenizer/tokenizer.h"

namespace {

using pl::mllm::DType;
using pl::mllm::fp16_to_fp32;
using pl::mllm::GGUFFile;
using pl::mllm::ModelConfig;
using pl::mllm::ScratchArena;
using pl::mllm::Status;
using pl::mllm::TensorView;

// Q8_0 block layout (ggml-compatible, little-endian).
struct Q8Block {
    uint16_t scale; // fp16
    int8_t qs[32];
};
static_assert(sizeof(Q8Block) == pl::mllm::kQ8_0TypeSize);

// Copy the embedding row for `token` into `out` as f32.
Status embedding_row(const TensorView& embd, int32_t token, int32_t hidden, TensorView out) {
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
        const int64_t blocks_per_row = hidden / pl::mllm::kQ8_0BlockSize;
        const auto* blocks = static_cast<const Q8Block*>(embd.data()) +
                             static_cast<size_t>(token) * static_cast<size_t>(blocks_per_row);
        for (int32_t i = 0; i < hidden; ++i) {
            const int64_t blk = i / pl::mllm::kQ8_0BlockSize;
            const int64_t in = i % pl::mllm::kQ8_0BlockSize;
            od[i] = fp16_to_fp32(blocks[blk].scale) * static_cast<float>(blocks[blk].qs[in]);
        }
        return {};
    }
    return Status::Error(pl::mllm::ErrorCode::kUnsupported, "embedding: unsupported dtype");
}

// Print l2 norm, max abs and nan/inf counts of a f32 view.
void dump_stats(const char* tag, const TensorView& v) {
    const float* d = v.data_as<const float>();
    const int64_t n = v.shape().numel();
    double sum_sq = 0.0;
    float max_abs = 0.0f;
    int nan_count = 0;
    int inf_count = 0;
    for (int64_t i = 0; i < n; ++i) {
        const float x = d[i];
        if (std::isnan(x)) {
            ++nan_count;
        } else if (std::isinf(x)) {
            ++inf_count;
        } else {
            sum_sq += static_cast<double>(x) * x;
            max_abs = std::max(max_abs, std::fabs(x));
        }
    }
    std::printf("%-24s n=%-8lld l2=%-12.4f maxabs=%-12.4g nan=%-4d inf=%d\n",
                tag,
                static_cast<long long>(n),
                std::sqrt(sum_sq),
                max_abs,
                nan_count,
                inf_count);
}

// Find a named weight view; empty view when absent.
TensorView find_w(const std::vector<pl::mllm::Model::WeightEntry>& entries, std::string_view name) {
    for (const auto& e : entries) {
        if (e.name == name) {
            return e.view;
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    std::string model_path;
    std::string prompt = "Hello";
    int32_t max_context = 256;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prompt = argv[++i];
        } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            max_context = std::atoi(argv[++i]);
        }
    }
    if (model_path.empty()) {
        std::fprintf(stderr, "usage: %s -m <model.gguf> [-p prompt] [-c ctx]\n", argv[0]);
        return 1;
    }

    // ---- Load GGUF ----
    auto gguf_result = GGUFFile::Open(model_path);
    if (!gguf_result.ok()) {
        std::fprintf(stderr, "open failed: %s\n", gguf_result.status().message.c_str());
        return 1;
    }
    auto gguf = std::move(gguf_result).value();

    auto cfg_result = gguf->model_config();
    if (!cfg_result.ok()) {
        std::fprintf(stderr, "config failed: %s\n", cfg_result.status().message.c_str());
        return 1;
    }
    const ModelConfig cfg = cfg_result.value();
    std::printf("arch=%s vocab=%d hidden=%d inter=%d layers=%d heads=%d kv_heads=%d "
                "head_dim=%d ctx=%d rope_base=%g eps=%g qkv_bias=%d qk_norm=%d\n",
                cfg.architecture.c_str(),
                cfg.vocab_size,
                cfg.hidden_size,
                cfg.intermediate_size,
                cfg.num_layers,
                cfg.num_attention_heads,
                cfg.num_kv_heads,
                cfg.effective_head_dim(),
                cfg.context_length,
                cfg.rope_freq_base,
                cfg.rms_norm_eps,
                cfg.qkv_bias ? 1 : 0,
                cfg.qk_norm ? 1 : 0);

    // ---- Collect weight entries ----
    std::vector<pl::mllm::Model::WeightEntry> entries;
    for (const auto& ti : gguf->tensors()) {
        auto view = gguf->tensor(ti.name);
        if (view.ok()) {
            entries.push_back({ti.name, view.value()});
        }
    }

    // ---- Build model + backend ----
    auto model_result = pl::mllm::CreateModel(cfg, entries);
    if (!model_result.ok()) {
        std::fprintf(stderr, "create model failed: %s\n", model_result.status().message.c_str());
        return 1;
    }
    auto model = std::move(model_result).value();

    pl::mllm::CpuBackend backend;
    {
        std::vector<TensorView> views;
        std::vector<std::string_view> names;
        for (const auto& e : entries) {
            views.push_back(e.view);
            names.push_back(e.name);
        }
        if (auto s = backend.ImportWeights(views, names); !s.ok()) {
            std::fprintf(stderr, "import weights failed: %s\n", s.message.c_str());
            return 1;
        }
    }

    auto cache_result = pl::mllm::KVCache::Create(cfg, max_context, DType::kF32);
    if (!cache_result.ok()) {
        std::fprintf(stderr, "kv cache failed: %s\n", cache_result.status().message.c_str());
        return 1;
    }
    auto cache = std::move(cache_result).value();

    const size_t arena_bytes =
        static_cast<size_t>(
            std::max(cfg.intermediate_size, cfg.num_attention_heads * cfg.effective_head_dim())) *
            10 * 4 * static_cast<size_t>(cfg.num_layers) * 2 +
        65536;
    auto arena_result = ScratchArena::Create(arena_bytes);
    if (!arena_result.ok()) {
        std::fprintf(stderr, "arena failed: %s\n", arena_result.status().message.c_str());
        return 1;
    }
    auto arena = std::move(arena_result).value();

    auto embd_view = gguf->tensor("token_embd.weight");
    if (!embd_view.ok()) {
        std::fprintf(stderr, "no token_embd.weight\n");
        return 1;
    }

    // ---- Tokenize ----
    auto tok_result = pl::mllm::Tokenizer::FromGGUF(*gguf);
    if (!tok_result.ok()) {
        std::fprintf(stderr, "tokenizer failed: %s\n", tok_result.status().message.c_str());
        return 1;
    }
    auto tokenizer = std::move(tok_result).value();
    auto enc = tokenizer.Encode(prompt, true);
    if (!enc.ok()) {
        std::fprintf(stderr, "encode failed: %s\n", enc.status().message.c_str());
        return 1;
    }
    const auto ids = enc.value();
    std::printf("\n-- prompt: %zu tokens --\n", ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        auto p = tokenizer.DecodeOne(ids[i]);
        std::printf("  [%zu] id=%-7d piece=%s\n", i, ids[i], p.ok() ? p.value().c_str() : "?");
    }

    // ---- Prefill (manual per-layer loop to dump per-layer activation stats) ----
    // The hidden buffer must live OUTSIDE the scratch arena: every layer resets
    // the arena and would otherwise clobber the residual stream.
    const int32_t hidden = cfg.hidden_size;
    auto hidden_buf = pl::mllm::OwnedBuffer::AllocateCpu(static_cast<size_t>(hidden) * 4, 64);
    if (!hidden_buf.ok()) {
        std::fprintf(stderr, "hidden alloc failed: %s\n", hidden_buf.status().message.c_str());
        return 1;
    }
    auto hidden_owned = std::move(hidden_buf).value();
    TensorView hidden_view(hidden_owned.data(), DType::kF32, {1, hidden});

    TensorView last_hidden;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (auto s = embedding_row(embd_view.value(), ids[i], hidden, hidden_view); !s.ok()) {
            std::fprintf(stderr, "embedding failed: %s\n", s.message.c_str());
            return 1;
        }
        const bool last_token = (i == ids.size() - 1);
        for (int32_t l = 0; l < cfg.num_layers; ++l) {
            const auto names = pl::mllm::make_layer_weight_names(l, cfg.qkv_bias, cfg.qk_norm);
            pl::mllm::LayerWeights lw;
            lw.q_weight_name = names.q_weight;
            lw.k_weight_name = names.k_weight;
            lw.v_weight_name = names.v_weight;
            lw.o_weight_name = names.o_weight;
            lw.gate_weight_name = names.gate_weight;
            lw.up_weight_name = names.up_weight;
            lw.down_weight_name = names.down_weight;
            lw.attn_norm = find_w(entries, names.attn_norm);
            lw.mlp_norm = find_w(entries, names.mlp_norm);
            lw.q_bias = find_w(entries, names.q_bias);
            lw.k_bias = find_w(entries, names.k_bias);
            lw.v_bias = find_w(entries, names.v_bias);
            lw.q_norm = find_w(entries, names.q_norm);
            lw.k_norm = find_w(entries, names.k_norm);
            arena.Reset();
            const pl::mllm::TransformerLayer layer(l, lw);
            auto s =
                layer.Forward(hidden_view, static_cast<int64_t>(i), cache, backend, arena, cfg);
            if (!s.ok()) {
                std::fprintf(stderr, "layer %d failed: %s\n", l, s.message.c_str());
                return 1;
            }
            if (last_token) {
                char tag[32];
                std::snprintf(tag, sizeof(tag), "layer %02d out", l);
                dump_stats(tag, hidden_view);
            }
        }
        cache.Advance();
        last_hidden = hidden_view;
        if (last_token) {
            dump_stats("last hidden", last_hidden);
        }
    }

    // ---- Logits ----
    auto logits_buf =
        pl::mllm::OwnedBuffer::AllocateCpu(static_cast<size_t>(cfg.vocab_size) * 4, 64);
    if (!logits_buf.ok()) {
        std::fprintf(stderr, "logits alloc failed: %s\n", logits_buf.status().message.c_str());
        return 1;
    }
    auto logits_owned = std::move(logits_buf).value();
    TensorView logits(logits_owned.data(), DType::kF32, {1, cfg.vocab_size});

    arena.Reset();
    if (auto s = model->ComputeLogits(last_hidden, logits, backend, arena); !s.ok()) {
        std::fprintf(stderr, "compute logits failed: %s\n", s.message.c_str());
        return 1;
    }
    dump_stats("logits", logits);

    auto dump_top = [&](const char* tag, int n) {
        const float* lp = logits.data_as<const float>();
        std::vector<int32_t> idx(static_cast<size_t>(cfg.vocab_size));
        for (int32_t t = 0; t < cfg.vocab_size; ++t) {
            idx[static_cast<size_t>(t)] = t;
        }
        const int32_t k = std::min(n, cfg.vocab_size);
        std::partial_sort(idx.begin(), idx.begin() + k, idx.end(), [&](int32_t a, int32_t b) {
            return lp[a] > lp[b];
        });
        std::printf("\n-- %s --\n", tag);
        for (int32_t t = 0; t < k; ++t) {
            const int32_t id = idx[static_cast<size_t>(t)];
            auto piece = tokenizer.DecodeOne(id);
            std::printf("  %2d. id=%-6d logit=%-10.4f piece=%s\n",
                        t,
                        id,
                        lp[id],
                        piece.ok() ? piece.value().c_str() : "?");
        }
    };
    dump_top("top-10 logits", 10);

    // ---- Greedy decode dump (a few steps to inspect generation health) ----
    int64_t pos = static_cast<int64_t>(ids.size());
    int32_t last = -1;
    {
        const float* lp = logits.data_as<const float>();
        int32_t best = 0;
        for (int32_t t = 1; t < cfg.vocab_size; ++t) {
            if (lp[t] > lp[best]) {
                best = t;
            }
        }
        last = best;
    }
    for (int32_t step = 0; step < 4; ++step) {
        if (auto s = embedding_row(embd_view.value(), last, hidden, hidden_view); !s.ok()) {
            std::fprintf(stderr, "decode embed failed: %s\n", s.message.c_str());
            return 1;
        }
        for (int32_t l = 0; l < cfg.num_layers; ++l) {
            const auto names = pl::mllm::make_layer_weight_names(l, cfg.qkv_bias, cfg.qk_norm);
            pl::mllm::LayerWeights lw;
            lw.q_weight_name = names.q_weight;
            lw.k_weight_name = names.k_weight;
            lw.v_weight_name = names.v_weight;
            lw.o_weight_name = names.o_weight;
            lw.gate_weight_name = names.gate_weight;
            lw.up_weight_name = names.up_weight;
            lw.down_weight_name = names.down_weight;
            lw.attn_norm = find_w(entries, names.attn_norm);
            lw.mlp_norm = find_w(entries, names.mlp_norm);
            lw.q_bias = find_w(entries, names.q_bias);
            lw.k_bias = find_w(entries, names.k_bias);
            lw.v_bias = find_w(entries, names.v_bias);
            lw.q_norm = find_w(entries, names.q_norm);
            lw.k_norm = find_w(entries, names.k_norm);
            arena.Reset();
            const pl::mllm::TransformerLayer layer(l, lw);
            auto s = layer.Forward(hidden_view, pos, cache, backend, arena, cfg);
            if (!s.ok()) {
                std::fprintf(stderr, "decode layer %d failed: %s\n", l, s.message.c_str());
                return 1;
            }
        }
        cache.Advance();
        ++pos;

        arena.Reset();
        if (auto s = model->ComputeLogits(hidden_view, logits, backend, arena); !s.ok()) {
            std::fprintf(stderr, "decode logits failed: %s\n", s.message.c_str());
            return 1;
        }
        auto piece = tokenizer.DecodeOne(last);
        std::printf(
            "\n-- decode step %d (in=%s) --\n", step, piece.ok() ? piece.value().c_str() : "?");
        {
            const float* lp = logits.data_as<const float>();
            std::vector<int32_t> ids2(static_cast<size_t>(cfg.vocab_size));
            for (int32_t t = 0; t < cfg.vocab_size; ++t) {
                ids2[static_cast<size_t>(t)] = t;
            }
            std::partial_sort(ids2.begin(),
                              ids2.begin() + 5,
                              ids2.end(),
                              [&](int32_t a, int32_t b) { return lp[a] > lp[b]; });
            for (int32_t t = 0; t < 5; ++t) {
                const int32_t id = ids2[static_cast<size_t>(t)];
                auto p = tokenizer.DecodeOne(id);
                std::printf("  %d. id=%-6d logit=%-10.4f piece=%s\n",
                            t,
                            id,
                            lp[id],
                            p.ok() ? p.value().c_str() : "?");
            }
            last = ids2[0];
        }
    }

    return 0;
}
