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

// End-to-end engine test: builds a tiny GGUF model in memory, loads it through
// the Engine, and verifies that prefill + decode produce deterministic output.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/ut/testdata/gguf_writer.h"
#include "gtest/gtest.h"

namespace pl::mllm {
namespace {

namespace td = pl::mllm::testdata;

// Temp file RAII wrapper

class TempFile {
public:
    explicit TempFile(std::vector<uint8_t> bytes)
        : path_(std::filesystem::temp_directory_path() / "mllm_engine_test_XXXXXX") {
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() { std::filesystem::remove(path_); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// GGUF float encoding helpers

std::vector<uint8_t> f32_bytes(const std::vector<float>& values) {
    std::vector<uint8_t> out;
    out.reserve(values.size() * 4);
    for (float v : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        out.push_back(static_cast<uint8_t>(bits & 0xFF));
        out.push_back(static_cast<uint8_t>(bits >> 8));
        out.push_back(static_cast<uint8_t>(bits >> 16));
        out.push_back(static_cast<uint8_t>(bits >> 24));
    }
    return out;
}

// Q4_0 encoding helper (ggml layout): each 32-element block becomes 18
// bytes {fp16 scale, 16 packed nibbles}; value = (nibble - 8) * scale.
std::vector<uint8_t> q4_0_bytes(const std::vector<float>& values) {
    const size_t num_blocks = values.size() / 32;
    std::vector<uint8_t> out;
    out.reserve(num_blocks * 18);
    auto push16 = [&out](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    for (size_t blk = 0; blk < num_blocks; ++blk) {
        const float* src = values.data() + blk * 32;
        float max_abs = 0.0f;
        for (int j = 0; j < 32; ++j) {
            max_abs = std::max(max_abs, std::abs(src[j]));
        }
        const float scale = max_abs >= 1e-8f ? max_abs / -8.0f : 1.0f;
        push16(fp32_to_fp16(scale));
        for (int j = 0; j < 16; ++j) {
            const int q0 = std::max(-8, std::min(7, static_cast<int>(std::lround(src[j] / scale))));
            const int q1 =
                std::max(-8, std::min(7, static_cast<int>(std::lround(src[j + 16] / scale))));
            out.push_back(static_cast<uint8_t>((q0 + 8) | ((q1 + 8) << 4)));
        }
    }
    return out;
}

// Tiny model configuration
//
// hidden_size     = 16  (2 heads * 8 head_dim)
// intermediate    = 32
// num_layers      = 1
// vocab_size      = 8
// context_length  = 64
// num_heads       = 2
// num_kv_heads    = 1
//
// Weight tensors (all F32 for deterministic CPU compute).
// GGML stores dims in column-major order (dims[0] = contiguous axis).
// GGUFFile reverses dims on load -> row-major [out_dim, in_dim] for MatMul.
//   token_embd.weight           ggml {hidden, vocab} -> loaded [vocab, hidden] = [8, 16]
//   output_norm.weight          ggml {hidden}         -> [16]
//   blk.0.attn_norm.weight      ggml {hidden}         -> [16]
//   blk.0.attn_q.weight         ggml {hidden, hidden} -> [16, 16]
//   blk.0.attn_k.weight         ggml {hidden, kv_dim} -> [kv_dim, hidden] = [8, 16]
//   blk.0.attn_v.weight         ggml {hidden, kv_dim} -> [kv_dim, hidden] = [8, 16]
//   blk.0.attn_output.weight    ggml {hidden, hidden} -> [16, 16]
//   blk.0.ffn_norm.weight       ggml {hidden}         -> [16]
//   blk.0.ffn_gate.weight       ggml {hidden, inter} -> [inter, hidden] = [32, 16]
//   blk.0.ffn_up.weight         ggml {hidden, inter} -> [inter, hidden] = [32, 16]
//   blk.0.ffn_down.weight       ggml {inter, hidden}  -> [hidden, inter] = [16, 32]

constexpr int32_t kHidden = 16;
constexpr int32_t kInter = 32;
constexpr int32_t kVocabSize = 8;
constexpr int32_t kCtx = 64;
constexpr int32_t kHeads = 2;
constexpr int32_t kKVHeads = 1;
constexpr int32_t kHeadDim = 8;

// Deterministic seed for weight generation (not the sampler seed - this
// controls the weight values so golden logits are reproducible).
constexpr uint32_t kWeightSeed = 42;

// Simple LCG for reproducible weight values.
float lcg_next(uint32_t& state) {
    state = state * 1103515245u + 12345u;
    return static_cast<float>(state) / static_cast<float>(0xFFFFFFFFu) * 2.0f - 1.0f;
}

std::vector<float> make_weights(uint32_t seed, size_t count) {
    uint32_t state = seed;
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = lcg_next(state) * 0.1f; // small init for stable softmax
    }
    return out;
}

// All-ones for norm weights (so RMSNorm is just x / sqrt(mean(x^2) + eps)).
std::vector<float> ones(size_t count) {
    return std::vector<float>(count, 1.0f);
}

// Tokenizer vocab: 8 tokens including byte-fallback entries

const std::vector<std::string> kVocab = {
    "<s>",    // 0: BOS
    "</s>",   // 1: EOS
    " ",      // 2: space
    "a",      // 3
    "b",      // 4
    "c",      // 5
    "<0x00>", // 6: byte fallback
    "<0x01>", // 7: byte fallback
};

td::GgufWriter make_tiny_model_writer(std::string arch = "llama") {
    // Family-specific extras (mirrors the arch registry feature flags):
    //   qwen2 -> additive Q/K/V projection biases
    //   qwen3 -> per-head Q/K RMSNorm before RoPE
    const bool qkv_bias = arch == "qwen2";
    const bool qk_norm = arch == "qwen3";
    const float rope_base = arch == "llama" ? 10000.0f : 1000000.0f;

    td::GgufWriter w(arch);
    w.meta_u32(arch + ".context_length", kCtx);
    w.meta_u32(arch + ".embedding_length", kHidden);
    w.meta_u32(arch + ".feed_forward_length", kInter);
    w.meta_u32(arch + ".block_count", 1);
    w.meta_u32(arch + ".attention.head_count", kHeads);
    w.meta_u32(arch + ".attention.head_count_kv", kKVHeads);
    w.meta_f32(arch + ".attention.layer_norm_rms_epsilon", 1e-5f);
    w.meta_f32(arch + ".rope.freq_base", rope_base);

    // Tokenizer metadata.
    w.meta_str_array("tokenizer.ggml.tokens", kVocab);
    w.meta_f32_array("tokenizer.ggml.scores",
                     {0.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f});
    w.meta_bool("tokenizer.ggml.add_bos_token", true);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 1);

    // Token embedding: ggml dims {hidden, vocab} -> loaded [vocab, hidden] = [8, 16]
    // Each row is one token's embedding vector of length hidden.
    // Also used as tied output weight: [vocab, hidden] = [out_dim, in_dim] for MatMul.
    w.tensor({"token_embd.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kVocabSize)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed, kHidden * kVocabSize))});

    // Output norm: [hidden]
    w.tensor({"output_norm.weight",
              {static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden))});

    // Layer 0 weights.
    const std::string p = "blk.0.";

    // Attention norm: [hidden]
    w.tensor({p + "attn_norm.weight",
              {static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden))});

    // Q/K/V/O: ggml dims {in_dim, out_dim} -> loaded [out_dim, in_dim] for MatMul.
    // Q: ggml {hidden, hidden} -> [16, 16]
    w.tensor({p + "attn_q.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 1, kHidden * kHidden))});

    // K: ggml {hidden, kv_dim} -> [kv_dim, hidden] = [8, 16]
    w.tensor({p + "attn_k.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kKVHeads * kHeadDim)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 2, kKVHeads * kHeadDim * kHidden))});

    // V: ggml {hidden, kv_dim} -> [kv_dim, hidden] = [8, 16]
    w.tensor({p + "attn_v.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kKVHeads * kHeadDim)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 3, kKVHeads * kHeadDim * kHidden))});

    // O: ggml {hidden, hidden} -> [16, 16]
    w.tensor({p + "attn_output.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 4, kHidden * kHidden))});

    // FFN norm: [hidden]
    w.tensor({p + "ffn_norm.weight",
              {static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden))});

    // Gate: ggml {hidden, inter} -> [inter, hidden] = [32, 16]
    w.tensor({p + "ffn_gate.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kInter)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 5, kInter * kHidden))});

    // Up: ggml {hidden, inter} -> [inter, hidden] = [32, 16]
    w.tensor({p + "ffn_up.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kInter)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 6, kInter * kHidden))});

    // Down: ggml {inter, hidden} -> [hidden, inter] = [16, 32]
    w.tensor({p + "ffn_down.weight",
              {static_cast<uint64_t>(kInter), static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 7, kHidden * kInter))});

    if (qkv_bias) {
        w.tensor({p + "attn_q.bias",
                  {static_cast<uint64_t>(kHeads * kHeadDim)},
                  td::GgufType::kF32,
                  f32_bytes(make_weights(kWeightSeed + 8, kHeads * kHeadDim))});
        w.tensor({p + "attn_k.bias",
                  {static_cast<uint64_t>(kKVHeads * kHeadDim)},
                  td::GgufType::kF32,
                  f32_bytes(make_weights(kWeightSeed + 9, kKVHeads * kHeadDim))});
        w.tensor({p + "attn_v.bias",
                  {static_cast<uint64_t>(kKVHeads * kHeadDim)},
                  td::GgufType::kF32,
                  f32_bytes(make_weights(kWeightSeed + 10, kKVHeads * kHeadDim))});
    }
    if (qk_norm) {
        w.tensor({p + "attn_q_norm.weight",
                  {static_cast<uint64_t>(kHeadDim)},
                  td::GgufType::kF32,
                  f32_bytes(ones(kHeadDim))});
        w.tensor({p + "attn_k_norm.weight",
                  {static_cast<uint64_t>(kHeadDim)},
                  td::GgufType::kF32,
                  f32_bytes(ones(kHeadDim))});
    }

    return w;
}

// Tests

TEST(EngineE2ETest, CreateLoadsTinyModel) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();
    EXPECT_EQ(engine->last_perf_stats().prompt_tokens, 0); // not yet run
}

TEST(EngineE2ETest, GreedyGenerateDeterministic) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 4;
    gp.temperature = 0.0f; // greedy
    gp.seed = 42;

    // Collect generated token ids.
    std::vector<int32_t> generated;
    auto status = engine->GenerateStream("a", gp, [&](std::string_view /*piece*/, int32_t tok) {
        generated.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status.ok()) << status.message;

    // Should generate at least 1 token (unless EOS immediately).
    EXPECT_GT(generated.size(), 0u);

    // Re-run with same seed: output must be identical (greedy = deterministic).
    std::vector<int32_t> generated2;
    auto status2 = engine->GenerateStream("a", gp, [&](std::string_view /*piece*/, int32_t tok) {
        generated2.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status2.ok()) << status2.message;

    EXPECT_EQ(generated, generated2);
}

TEST(EngineE2ETest, PerfStatsPopulatedAfterGenerate) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 2;
    gp.temperature = 0.0f;

    auto status = engine->GenerateStream("ab", gp, [](std::string_view, int32_t) { return true; });
    ASSERT_TRUE(status.ok()) << status.message;

    const auto& perf = engine->last_perf_stats();
    EXPECT_GT(perf.prompt_tokens, 0);
    EXPECT_GE(perf.generated_tokens, 0);
    EXPECT_GE(perf.prefill_ms, 0.0);
    EXPECT_GE(perf.decode_ms, 0.0);
    EXPECT_GT(perf.total_ms, 0.0);
}

// Metal end-to-end parity: the same tiny model run on CPU and Metal must emit
// identical greedy token sequences. Compiled (and registered) only on macOS;
// the Metal backend is unavailable elsewhere and `bazel test` on those
// platforms must not fail.
#if defined(__APPLE__)
TEST(EngineE2ETest, MetalGenerateMatchesCpu) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    Engine::Options cpu_opts;
    cpu_opts.model_path = tmp.path();
    cpu_opts.backend = BackendKind::kCpu;
    auto cpu_result = Engine::Create(cpu_opts);
    ASSERT_TRUE(cpu_result.ok()) << cpu_result.status().message;
    auto cpu_engine = std::move(cpu_result).value();

    GenerateParams gp;
    gp.max_tokens = 4;
    gp.temperature = 0.0f; // greedy -> deterministic
    gp.seed = 42;

    std::vector<int32_t> cpu_tokens;
    auto cpu_status = cpu_engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        cpu_tokens.push_back(tok);
        return true;
    });
    ASSERT_TRUE(cpu_status.ok()) << cpu_status.message;

    Engine::Options metal_opts;
    metal_opts.model_path = tmp.path();
    metal_opts.backend = BackendKind::kMetal;
    auto metal_result = Engine::Create(metal_opts);
    ASSERT_TRUE(metal_result.ok()) << metal_result.status().message;
    auto metal_engine = std::move(metal_result).value();

    std::vector<int32_t> metal_tokens;
    auto metal_status = metal_engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        metal_tokens.push_back(tok);
        return true;
    });
    if (!metal_status.ok()) {
        GTEST_SKIP() << "Metal backend unavailable: " << metal_status.message;
    }

    EXPECT_EQ(cpu_tokens, metal_tokens);
}
#endif // __APPLE__

// Same tiny model end-to-end under the qwen2 (QKV bias) and qwen3 (QK norm)
// families: the engine must load, and greedy decode must be deterministic.

TEST(EngineE2ETest, Qwen2CreateAndGenerate) {
    auto w = make_tiny_model_writer("qwen2");
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 4;
    gp.temperature = 0.0f;

    std::vector<int32_t> generated;
    auto status = engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        generated.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_GT(generated.size(), 0u);

    std::vector<int32_t> generated2;
    auto status2 = engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        generated2.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status2.ok()) << status2.message;
    EXPECT_EQ(generated, generated2);
}

TEST(EngineE2ETest, Qwen3CreateAndGenerate) {
    auto w = make_tiny_model_writer("qwen3");
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 4;
    gp.temperature = 0.0f;

    std::vector<int32_t> generated;
    auto status = engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        generated.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_GT(generated.size(), 0u);
}

// All-Q4_0 tiny model end-to-end: the loader, embedding lookup, every
// matmul (Q/K/V/O/gate/up/down + tied output), and greedy decode must all
// work with quantized weights, deterministically.
td::GgufWriter make_tiny_q4_model_writer() {
    // hidden = 32 keeps every matmul in-dim Q4_0 block aligned.
    constexpr int64_t kHidden4 = 32;
    constexpr int64_t kHeads4 = 4;
    constexpr int64_t kKVHeads4 = 2;
    constexpr int64_t kInter4 = 64;

    td::GgufWriter w("llama");
    w.meta_u32("llama.context_length", kCtx);
    w.meta_u32("llama.embedding_length", static_cast<uint32_t>(kHidden4));
    w.meta_u32("llama.feed_forward_length", static_cast<uint32_t>(kInter4));
    w.meta_u32("llama.block_count", 1);
    w.meta_u32("llama.attention.head_count", static_cast<uint32_t>(kHeads4));
    w.meta_u32("llama.attention.head_count_kv", static_cast<uint32_t>(kKVHeads4));
    w.meta_f32("llama.attention.layer_norm_rms_epsilon", 1e-5f);
    w.meta_f32("llama.rope.freq_base", 10000.0f);

    w.meta_str_array("tokenizer.ggml.tokens", kVocab);
    w.meta_f32_array("tokenizer.ggml.scores",
                     {0.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f});
    w.meta_bool("tokenizer.ggml.add_bos_token", true);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 1);

    auto q4_weight = [&w](std::string name,
                          std::initializer_list<uint64_t> ggml_dims,
                          uint32_t seed,
                          size_t count) {
        w.tensor({std::move(name),
                  std::vector<uint64_t>(ggml_dims),
                  td::GgufType::kQ4_0,
                  q4_0_bytes(make_weights(seed, count))});
    };

    // ggml dims {in, out}; loaded row-major becomes [out, in].
    // All in-dims (32 or 64) are Q4_0 block aligned. Embedding doubles as
    // the tied output weight.
    q4_weight("token_embd.weight",
              {static_cast<uint64_t>(kHidden4), static_cast<uint64_t>(kVocabSize)},
              kWeightSeed,
              kHidden4 * kVocabSize);
    w.tensor({"output_norm.weight",
              {static_cast<uint64_t>(kHidden4)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden4))});

    const std::string p = "blk.0.";
    w.tensor({p + "attn_norm.weight",
              {static_cast<uint64_t>(kHidden4)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden4))});
    q4_weight(p + "attn_q.weight", {32, 32}, kWeightSeed + 1, 32 * 32);
    q4_weight(p + "attn_k.weight",
              {32, static_cast<uint64_t>(kKVHeads4 * kHeadDim)},
              kWeightSeed + 2,
              16 * 32);
    q4_weight(p + "attn_v.weight",
              {32, static_cast<uint64_t>(kKVHeads4 * kHeadDim)},
              kWeightSeed + 3,
              16 * 32);
    q4_weight(p + "attn_output.weight", {32, 32}, kWeightSeed + 4, 32 * 32);
    w.tensor({p + "ffn_norm.weight",
              {static_cast<uint64_t>(kHidden4)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden4))});
    q4_weight(
        p + "ffn_gate.weight", {32, static_cast<uint64_t>(kInter4)}, kWeightSeed + 5, kInter4 * 32);
    q4_weight(
        p + "ffn_up.weight", {32, static_cast<uint64_t>(kInter4)}, kWeightSeed + 6, kInter4 * 32);
    q4_weight(
        p + "ffn_down.weight", {static_cast<uint64_t>(kInter4), 32}, kWeightSeed + 7, 32 * kInter4);
    return w;
}

TEST(EngineE2ETest, Q4EngineCreateAndGenerateDeterministic) {
    auto w = make_tiny_q4_model_writer();
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 4;
    gp.temperature = 0.0f; // greedy

    std::vector<int32_t> generated;
    auto status = engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        generated.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_GT(generated.size(), 0u);

    std::vector<int32_t> generated2;
    auto status2 = engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
        generated2.push_back(tok);
        return true;
    });
    ASSERT_TRUE(status2.ok()) << status2.message;
    EXPECT_EQ(generated, generated2);
}

// Ring (sliding-window) KV cache: sequences longer than the window must not
// fail — the oldest tokens' K/V are dropped behind a window origin and
// generation continues indefinitely.

TEST(EngineE2ETest, RingAllowsGenerationBeyondWindow) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    // Window of 8 tokens: prompt (2 incl. BOS) + 20 generated >> 8.
    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;
    opts.ring = true;
    opts.max_context = 8;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 20;
    gp.temperature = 0.0f;
    gp.seed = 42;

    auto run = [&](std::vector<int32_t>& out) {
        return engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
            out.push_back(tok);
            return true;
        });
    };

    std::vector<int32_t> generated;
    auto status = run(generated);
    ASSERT_TRUE(status.ok()) << status.message;
    // The prompt itself (2 tokens) already exceeds half the window, so the
    // sequence MUST have crossed the window boundary to produce this many.
    EXPECT_GT(generated.size(), 12u);

    // Same seed + greedy -> deterministic across runs.
    std::vector<int32_t> generated2;
    auto status2 = run(generated2);
    ASSERT_TRUE(status2.ok()) << status2.message;
    EXPECT_EQ(generated, generated2);
}

TEST(EngineE2ETest, RingStrictRejectsBeyondWindow) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;
    opts.ring = false; // strict MVP semantics
    opts.max_context = 8;

    auto result = Engine::Create(opts);
    ASSERT_TRUE(result.ok()) << result.status().message;
    auto engine = std::move(result).value();

    GenerateParams gp;
    gp.max_tokens = 20; // 2 prompt + 20 generated > 8 -> must be rejected
    gp.temperature = 0.0f;

    auto status = engine->GenerateStream("a", gp, [](std::string_view, int32_t) { return true; });
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code, ErrorCode::kInvalidArgument);
}

// Ring mode must be bit-identical to strict mode whenever the sequence fits
// inside the window (no compaction ever fires, so the physical layout and
// every computation are unchanged).
TEST(EngineE2ETest, RingMatchesStrictWithinWindow) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    auto run = [&](bool ring, std::vector<int32_t>& out) {
        Engine::Options opts;
        opts.model_path = tmp.path();
        opts.backend = BackendKind::kCpu;
        opts.ring = ring;
        opts.max_context = 32; // capacity 32 >= 2 prompt + 8 generated
        auto engine_result = Engine::Create(opts);
        ASSERT_TRUE(engine_result.ok()) << engine_result.status().message;
        auto engine = std::move(engine_result).value();

        GenerateParams gp;
        gp.max_tokens = 8;
        gp.temperature = 0.0f;
        gp.seed = 42;
        auto status = engine->GenerateStream("abc", gp, [&](std::string_view, int32_t tok) {
            out.push_back(tok);
            return true;
        });
        ASSERT_TRUE(status.ok()) << status.message;
    };

    std::vector<int32_t> strict_tokens;
    run(false, strict_tokens);
    std::vector<int32_t> ring_tokens;
    run(true, ring_tokens);
    EXPECT_EQ(strict_tokens, ring_tokens);
}

// Ring parity across backends: CPU (host cache, host compaction) and Metal
// (device KV, blit-driven ShiftKV) must emit identical greedy tokens even
// past the window, where both run the same shift policy.
#if defined(__APPLE__)
TEST(EngineE2ETest, RingMetalMatchesCpu) {
    auto w = make_tiny_model_writer();
    TempFile tmp(w.build(32));

    auto run = [&](BackendKind backend, std::vector<int32_t>& out) {
        Engine::Options opts;
        opts.model_path = tmp.path();
        opts.backend = backend;
        opts.ring = true;
        opts.max_context = 16; // 2 prompt + 26 generated > 16 -> shifting
        auto engine_result = Engine::Create(opts);
        ASSERT_TRUE(engine_result.ok()) << engine_result.status().message;
        auto engine = std::move(engine_result).value();

        GenerateParams gp;
        gp.max_tokens = 26;
        gp.temperature = 0.0f;
        gp.seed = 42;
        auto status = engine->GenerateStream("a", gp, [&](std::string_view, int32_t tok) {
            out.push_back(tok);
            return true;
        });
        if (!status.ok() && backend == BackendKind::kMetal) {
            GTEST_SKIP() << "Metal backend unavailable: " << status.message;
        }
        ASSERT_TRUE(status.ok()) << status.message;
    };

    std::vector<int32_t> cpu_tokens;
    run(BackendKind::kCpu, cpu_tokens);
    std::vector<int32_t> metal_tokens;
    run(BackendKind::kMetal, metal_tokens);
    EXPECT_EQ(cpu_tokens, metal_tokens);
}
#endif // __APPLE__

TEST(EngineE2ETest, UnsupportedArchitectureRejected) {
    // A llama-shaped metadata layout but a foreign arch prefix must be
    // rejected at Engine::Create time.
    td::GgufWriter bad("mamba");
    bad.meta_u32("mamba.context_length", kCtx);
    bad.meta_u32("mamba.embedding_length", kHidden);
    bad.meta_u32("mamba.feed_forward_length", kInter);
    bad.meta_u32("mamba.block_count", 1);
    bad.meta_u32("mamba.attention.head_count", kHeads);
    TempFile tmp(bad.build(32));

    Engine::Options opts;
    opts.model_path = tmp.path();
    opts.backend = BackendKind::kCpu;

    auto result = Engine::Create(opts);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace pl::mllm
