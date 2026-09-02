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

#include <algorithm>
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
#include "cpp/pl/mllm/model/dense_decoder.h"
#include "cpp/pl/mllm/model/model.h"
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
    std::vector<Model::WeightEntry> entries;

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

    const int32_t head_dim = cfg.effective_head_dim();
    for (int l = 0; l < cfg.num_layers; ++l) {
        auto names = make_layer_weight_names(l, cfg.qkv_bias, cfg.qk_norm);
        store.add_range(names.q_weight, cfg.num_attention_heads * head_dim, cfg.hidden_size);
        store.add_range(names.k_weight, cfg.num_kv_heads * head_dim, cfg.hidden_size);
        store.add_range(names.v_weight, cfg.num_kv_heads * head_dim, cfg.hidden_size);
        store.add_range(names.o_weight, cfg.hidden_size, cfg.num_attention_heads * head_dim);
        store.add_norm(names.attn_norm, cfg.hidden_size, 1.0f);
        store.add_range(names.gate_weight, cfg.intermediate_size, cfg.hidden_size);
        store.add_range(names.up_weight, cfg.intermediate_size, cfg.hidden_size);
        store.add_range(names.down_weight, cfg.hidden_size, cfg.intermediate_size);
        store.add_norm(names.mlp_norm, cfg.hidden_size, 1.0f);
        if (cfg.qkv_bias) {
            store.add_norm(names.q_bias, cfg.num_attention_heads * head_dim, 0.01f);
            store.add_norm(names.k_bias, cfg.num_kv_heads * head_dim, 0.01f);
            store.add_norm(names.v_bias, cfg.num_kv_heads * head_dim, 0.01f);
        }
        if (cfg.qk_norm) {
            store.add_norm(names.q_norm, head_dim, 1.0f);
            store.add_norm(names.k_norm, head_dim, 1.0f);
        }
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

    auto model_result = DenseDecoderModel::Create(cfg, store.entries);
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

    auto model_result = DenseDecoderModel::Create(cfg, store.entries);
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

// Architecture coverage: qwen2 (QKV bias) and qwen3 (QK norm + decoupled
// head_dim).

TEST(ModelTest, ConfigValidateArchitectures) {
    auto qwen2 = make_tiny_config();
    qwen2.architecture = "qwen2";
    qwen2.qkv_bias = true;
    EXPECT_TRUE(qwen2.Validate().ok());

    auto qwen3 = make_tiny_config();
    qwen3.architecture = "qwen3";
    qwen3.qk_norm = true;
    // Qwen3 decouples head_dim from hidden/heads (e.g. 0.6B: hidden 1024,
    // 16 heads, head_dim 128).
    qwen3.head_dim = 8; // 4 heads * 8 != hidden 16
    EXPECT_TRUE(qwen3.Validate().ok());

    auto unknown = make_tiny_config();
    unknown.architecture = "deepseek";
    EXPECT_FALSE(unknown.Validate().ok());

    // Explicit head_dim still must be even (RoPE rotation pairs).
    auto bad_hd = make_tiny_config();
    bad_hd.architecture = "qwen3";
    bad_hd.head_dim = 7;
    EXPECT_FALSE(bad_hd.Validate().ok());
}

TEST(ModelTest, ArchRegistryFeatureFlags) {
    EXPECT_EQ(find_architecture("llama"), &kArchLlama);
    EXPECT_EQ(find_architecture("qwen2"), &kArchQwen2);
    EXPECT_EQ(find_architecture("qwen3"), &kArchQwen3);
    EXPECT_TRUE(kArchQwen2.qkv_bias);
    EXPECT_FALSE(kArchQwen2.qk_norm);
    EXPECT_TRUE(kArchQwen3.qk_norm);
    EXPECT_FALSE(kArchQwen3.qkv_bias);
    EXPECT_GT(kArchQwen2.default_rope_freq_base, kArchLlama.default_rope_freq_base);
}

TEST(ModelTest, Qwen2ForwardWithBias) {
    auto cfg = make_tiny_config();
    cfg.architecture = "qwen2";
    cfg.qkv_bias = true;
    auto store = make_all_weights(cfg);

    auto model_result = CreateModel(cfg, store.entries);
    ASSERT_TRUE(model_result.ok()) << model_result.status().message;
    auto model = std::move(model_result).value();
    EXPECT_EQ(model->num_layers(), cfg.num_layers);

    CpuBackend backend;
    ASSERT_TRUE(import_weights(backend, store));

    auto cache = KVCache::Create(cfg, 32, DType::kF32).value();
    auto arena = ScratchArena::Create(1024 * 1024).value();

    auto hidden0 = store.entries[0].view.slice(0, 2, 3).value().reshape({1, cfg.hidden_size});
    ASSERT_TRUE(hidden0.ok());
    arena.Reset();
    ASSERT_TRUE(model->Forward(hidden0.value(), 0, cache, backend, arena).ok());

    arena.Reset();
    auto logits_buf = OwnedBuffer::AllocateCpu(cfg.vocab_size * 4, 64);
    ASSERT_TRUE(logits_buf.ok());
    auto logits_owned = std::move(logits_buf).value();
    TensorView logits(logits_owned.data(), DType::kF32, {1, cfg.vocab_size});
    ASSERT_TRUE(model->ComputeLogits(hidden0.value(), logits, backend, arena).ok());
    auto* lp = logits.data_as<float>();
    for (int i = 0; i < cfg.vocab_size; ++i) {
        EXPECT_TRUE(std::isfinite(lp[i]));
    }

    // weight_names() must cover the bias tensors so backends import them.
    const auto names = model->weight_names();
    EXPECT_NE(std::find(names.begin(), names.end(), "blk.0.attn_q.bias"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "blk.0.attn_v.bias"), names.end());
}

TEST(ModelTest, Qwen3ForwardWithQkNormAndDecoupledHeadDim) {
    auto cfg = make_tiny_config();
    cfg.architecture = "qwen3";
    cfg.qk_norm = true;
    cfg.head_dim = 8; // heads*head_dim (32) > hidden_size (16)
    cfg.rope_freq_base = 1000000.0f;
    auto store = make_all_weights(cfg);

    auto model_result = CreateModel(cfg, store.entries);
    ASSERT_TRUE(model_result.ok()) << model_result.status().message;
    auto model = std::move(model_result).value();

    CpuBackend backend;
    ASSERT_TRUE(import_weights(backend, store));

    auto cache = KVCache::Create(cfg, 32, DType::kF32).value();
    auto arena = ScratchArena::Create(1024 * 1024).value();

    auto hidden0 = store.entries[0].view.slice(0, 3, 4).value().reshape({1, cfg.hidden_size});
    ASSERT_TRUE(hidden0.ok());
    arena.Reset();
    ASSERT_TRUE(model->Forward(hidden0.value(), 0, cache, backend, arena).ok());

    auto hidden1 = store.entries[0].view.slice(0, 4, 5).value().reshape({1, cfg.hidden_size});
    ASSERT_TRUE(hidden1.ok());
    arena.Reset();
    ASSERT_TRUE(model->Forward(hidden1.value(), 1, cache, backend, arena).ok());

    arena.Reset();
    auto logits_buf = OwnedBuffer::AllocateCpu(cfg.vocab_size * 4, 64);
    ASSERT_TRUE(logits_buf.ok());
    auto logits_owned = std::move(logits_buf).value();
    TensorView logits(logits_owned.data(), DType::kF32, {1, cfg.vocab_size});
    ASSERT_TRUE(model->ComputeLogits(hidden1.value(), logits, backend, arena).ok());
    auto* lp = logits.data_as<float>();
    for (int i = 0; i < cfg.vocab_size; ++i) {
        EXPECT_TRUE(std::isfinite(lp[i]));
    }
}

TEST(ModelTest, Qwen2MissingBiasFailsFast) {
    auto cfg = make_tiny_config();
    cfg.architecture = "qwen2";
    cfg.qkv_bias = true;

    // Build llama-style weights (no bias) but claim qwen2 — Create must fail.
    cfg.qkv_bias = false;
    auto store = make_all_weights(cfg);
    cfg.qkv_bias = true;

    auto model_result = CreateModel(cfg, store.entries);
    EXPECT_FALSE(model_result.ok());
    EXPECT_EQ(model_result.status().code, ErrorCode::kNotFound);
}

// Regression: a Q4_0 checkpoint must fail fast at model creation with a
// clear error. Before the fix, Create() succeeded and the unsupported dtype
// surfaced only much later: matmul weights errored inside the first MatMul,
// while Q4_0 norm weights were read through the scalar elem_to_f32 path that
// silently returns 0 for Q4_0 (rmsnorm -> scale=0 -> all-zero hidden state).
TEST(ModelTest, CreateRejectsUnsupportedMatMulDtype) {
    auto cfg = make_tiny_config();
    auto store = make_all_weights(cfg);

    // 16x16 = 256 elements -> 8 Q4_0 blocks of 18 bytes.
    std::vector<uint8_t> q4_storage(dtype_nbytes(DType::kQ4_0, 16 * 16), 0);
    TensorView q4_view(q4_storage.data(), DType::kQ4_0, {16, 16});
    bool replaced = false;
    for (auto& e : store.entries) {
        if (e.name == "blk.0.attn_q.weight") {
            e.view = q4_view;
            replaced = true;
        }
    }
    ASSERT_TRUE(replaced);

    auto model_result = CreateModel(cfg, store.entries);
    ASSERT_FALSE(model_result.ok());
    EXPECT_EQ(model_result.status().code, ErrorCode::kUnsupported);
}

TEST(ModelTest, CreateRejectsUnsupportedNormDtype) {
    auto cfg = make_tiny_config();
    auto store = make_all_weights(cfg);

    // One full Q4_0 block (32 elements); Create() only checks the dtype.
    std::vector<uint8_t> q4_storage(dtype_nbytes(DType::kQ4_0, 32), 0);
    TensorView q4_view(q4_storage.data(), DType::kQ4_0, {32});
    bool replaced = false;
    for (auto& e : store.entries) {
        if (e.name == "blk.0.attn_norm.weight") {
            e.view = q4_view;
            replaced = true;
        }
    }
    ASSERT_TRUE(replaced);

    auto model_result = CreateModel(cfg, store.entries);
    ASSERT_FALSE(model_result.ok());
    EXPECT_EQ(model_result.status().code, ErrorCode::kUnsupported);
}

TEST(ModelTest, CreateRejectsMissingMatMulWeight) {
    auto cfg = make_tiny_config();
    auto store = make_all_weights(cfg);

    // Drop blk.1 down projection: previously accepted here, failing much
    // later inside MatMul with a backend-level "weight not found".
    store.entries.erase(std::remove_if(store.entries.begin(),
                                       store.entries.end(),
                                       [](const Model::WeightEntry& e) {
                                           return e.name == "blk.1.ffn_down.weight";
                                       }),
                        store.entries.end());

    auto model_result = CreateModel(cfg, store.entries);
    ASSERT_FALSE(model_result.ok());
    EXPECT_EQ(model_result.status().code, ErrorCode::kNotFound);
}

TEST(ModelTest, FactoryRejectsUnknownArchitecture) {
    auto cfg = make_tiny_config();
    cfg.architecture = "mamba";
    auto store = make_all_weights(make_tiny_config());
    auto model_result = CreateModel(cfg, store.entries);
    EXPECT_FALSE(model_result.ok());
    EXPECT_EQ(model_result.status().code, ErrorCode::kUnsupported);
}
