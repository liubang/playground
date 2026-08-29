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

#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/model/config.h"
#include "cpp/pl/mllm/model/llama_model.h"
#include "cpp/pl/mllm/model/transformer_layer.h"
#include "cpp/pl/mllm/model/weight_names.h"

using namespace pl::mllm;

namespace {

ModelConfig make_tiny_config() {
    return ModelConfig{
        .architecture = "llama",
        .vocab_size = 32,
        .hidden_size = 16,
        .intermediate_size = 32,
        .num_layers = 2,
        .num_attention_heads = 4,
        .num_kv_heads = 2,
        .head_dim = 4,
        .context_length = 64,
        .rms_norm_eps = 1e-5f,
        .rope_freq_base = 10000.0f,
    };
}

// Owns weight backing storage so TensorViews remain valid for the test lifetime.
struct WeightStore {
    std::vector<std::vector<float>> storage;
    std::vector<LlamaModel::WeightEntry> entries;

    TensorView add(std::string name, int rows, int cols, float fill) {
        auto& d = storage.emplace_back(static_cast<size_t>(rows) * static_cast<size_t>(cols), fill);
        TensorView v(d.data(), DType::kF32, {rows, cols});
        entries.push_back({std::move(name), v});
        return v;
    }

    TensorView add_range(std::string name, int rows, int cols) {
        auto& d = storage.emplace_back(static_cast<size_t>(rows) * static_cast<size_t>(cols));
        for (size_t i = 0; i < d.size(); ++i) {
            d[i] = static_cast<float>(i % 7) * 0.1f - 0.3f;
        }
        TensorView v(d.data(), DType::kF32, {rows, cols});
        entries.push_back({std::move(name), v});
        return v;
    }

    // Add a 1D norm weight from a 2D fill; reshaped to [hidden_size].
    TensorView add_norm(std::string name, int hidden_size, float fill) {
        auto& d = storage.emplace_back(static_cast<size_t>(hidden_size), fill);
        TensorView v(d.data(), DType::kF32, {hidden_size});
        entries.push_back({std::move(name), v});
        return v;
    }
};

WeightStore make_all_weights(const ModelConfig& cfg) {
    WeightStore store;

    store.add_range("token_embd.weight", cfg.vocab_size, cfg.hidden_size);
    store.add_norm("output_norm.weight", cfg.hidden_size, 1.0f);
    store.add_range("output.weight", cfg.vocab_size, cfg.hidden_size);

    for (int l = 0; l < cfg.num_layers; ++l) {
        auto names = make_layer_weight_names(l);
        store.add_range(names.q_weight, cfg.hidden_size, cfg.hidden_size);
        store.add_range(names.k_weight, cfg.num_kv_heads * cfg.head_dim, cfg.hidden_size);
        store.add_range(names.v_weight, cfg.num_kv_heads * cfg.head_dim, cfg.hidden_size);
        store.add_range(names.o_weight, cfg.hidden_size, cfg.hidden_size);
        store.add_norm(names.attn_norm, cfg.hidden_size, 1.0f);
        store.add_range(names.gate_weight, cfg.intermediate_size, cfg.hidden_size);
        store.add_range(names.up_weight, cfg.intermediate_size, cfg.hidden_size);
        store.add_range(names.down_weight, cfg.hidden_size, cfg.intermediate_size);
        store.add_norm(names.mlp_norm, cfg.hidden_size, 1.0f);
    }
    return store;
}

// Helper to import weights into a CpuBackend from a WeightStore.
bool import_weights(CpuBackend& backend, const WeightStore& store) {
    std::vector<TensorView> views;
    std::vector<std::string_view> names;
    for (const auto& e : store.entries) {
        views.push_back(e.view);
        names.push_back(e.name);
    }
    return backend.ImportWeights(views, names).ok();
}

} // namespace

TEST(ModelTest, ConfigValidate) {
    auto cfg = make_tiny_config();
    EXPECT_TRUE(cfg.Validate().ok());
}

TEST(ModelTest, ModelCreateAndForward) {
    auto cfg = make_tiny_config();
    auto store = make_all_weights(cfg);

    auto model_result = LlamaModel::Create(cfg, store.entries);
    ASSERT_TRUE(model_result.ok()) << model_result.status().message;
    auto model = std::move(model_result).value();

    CpuBackend backend;
    ASSERT_TRUE(import_weights(backend, store));

    auto cache_result = KVCache::Create(cfg, 32, DType::kF32);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    auto arena_result = ScratchArena::Create(1024 * 1024);
    ASSERT_TRUE(arena_result.ok());
    auto arena = std::move(arena_result).value();

    // Embedding: look up token 5 from the token_embd weight (index 0 in store).
    auto embd_view = store.entries[0].view; // token_embd.weight
    auto slice = embd_view.slice(0, 5, 6).value();
    auto hidden = slice.reshape({1, cfg.hidden_size}).value();

    arena.Reset();
    auto s = model->Forward(hidden, 0, cache, backend, arena);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(cache.length(), 1);

    auto logits_buf = OwnedBuffer::AllocateCpu(cfg.vocab_size * 4, 64);
    ASSERT_TRUE(logits_buf.ok());
    auto logits_owned = std::move(logits_buf).value();
    TensorView logits(logits_owned.data(), DType::kF32, {1, cfg.vocab_size});

    arena.Reset();
    s = model->ComputeLogits(hidden, logits, backend, arena);
    ASSERT_TRUE(s.ok()) << s.message;

    auto* lp = logits.data_as<float>();
    for (int i = 0; i < cfg.vocab_size; ++i) {
        EXPECT_TRUE(std::isfinite(lp[i])) << "logit[" << i << "] = " << lp[i];
    }
}

TEST(ModelTest, ModelForwardTwoTokens) {
    auto cfg = make_tiny_config();
    auto store = make_all_weights(cfg);

    auto model_result = LlamaModel::Create(cfg, store.entries);
    ASSERT_TRUE(model_result.ok());
    auto model = std::move(model_result).value();

    CpuBackend backend;
    ASSERT_TRUE(import_weights(backend, store));

    auto cache_result = KVCache::Create(cfg, 32, DType::kF32);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    auto arena_result = ScratchArena::Create(1024 * 1024);
    ASSERT_TRUE(arena_result.ok());
    auto arena = std::move(arena_result).value();

    auto embd_view = store.entries[0].view; // token_embd.weight
    auto slice0 = embd_view.slice(0, 0, 1).value();
    auto hidden0 = slice0.reshape({1, cfg.hidden_size}).value();

    arena.Reset();
    ASSERT_TRUE(model->Forward(hidden0, 0, cache, backend, arena).ok());
    EXPECT_EQ(cache.length(), 1);

    auto slice1 = embd_view.slice(0, 1, 2).value();
    auto hidden1 = slice1.reshape({1, cfg.hidden_size}).value();

    arena.Reset();
    ASSERT_TRUE(model->Forward(hidden1, 1, cache, backend, arena).ok());
    EXPECT_EQ(cache.length(), 2);

    auto logits_buf = OwnedBuffer::AllocateCpu(cfg.vocab_size * 4, 64);
    ASSERT_TRUE(logits_buf.ok());
    auto logits_owned = std::move(logits_buf).value();
    TensorView logits(logits_owned.data(), DType::kF32, {1, cfg.vocab_size});

    arena.Reset();
    ASSERT_TRUE(model->ComputeLogits(hidden1, logits, backend, arena).ok());

    auto* lp = logits.data_as<float>();
    for (int i = 0; i < cfg.vocab_size; ++i) {
        EXPECT_TRUE(std::isfinite(lp[i])) << "logit[" << i << "] = " << lp[i];
    }
}
