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

// Generates a tiny GGUF file (1 layer, 16 hidden, 8 vocab) for any supported
// architecture with deterministic constant weights, so the CLI / bench tools
// can be run end-to-end without a real model. Usage:
//
//   bazel run //cpp/pl/mllm/tools:make_tiny_model -- [-o out.gguf] [--arch llama|qwen2|qwen3]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/ut/testdata/gguf_writer.h"

namespace {

namespace td = pl::mllm::testdata;

constexpr int32_t kHidden = 16;
constexpr int32_t kInter = 32;
constexpr int32_t kVocab = 8;
constexpr int32_t kHeads = 2;
constexpr int32_t kKVHeads = 1;
constexpr int32_t kHeadDim = 8;

std::vector<uint8_t> f32_bytes(const std::vector<float>& values) {
    std::vector<uint8_t> out;
    out.reserve(values.size() * 4);
    for (float v : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<uint8_t>(bits >> (8 * k)));
        }
    }
    return out;
}

// Family-specific weight layout (mirrors the arch registry feature flags):
//   qwen2 -> additive Q/K/V projection biases
//   qwen3 -> per-head Q/K RMSNorm before RoPE
td::GgufWriter build_writer(std::string_view arch) {
    const bool qkv_bias = arch == "qwen2";
    const bool qk_norm = arch == "qwen3";
    const float rope_base = arch == "llama" ? 10000.0f : 1000000.0f;

    const std::string a(arch);
    td::GgufWriter w(a);
    w.meta_u32(a + ".context_length", 32);
    w.meta_u32(a + ".embedding_length", kHidden);
    w.meta_u32(a + ".feed_forward_length", kInter);
    w.meta_u32(a + ".block_count", 1);
    w.meta_u32(a + ".attention.head_count", kHeads);
    w.meta_u32(a + ".attention.head_count_kv", kKVHeads);
    w.meta_f32(a + ".attention.layer_norm_rms_epsilon", 1e-5f);
    w.meta_f32(a + ".rope.freq_base", rope_base);

    w.meta_str_array("tokenizer.ggml.tokens", {"<s>", "</s>", "a", "b", "c", " ", "d", "e"});
    w.meta_f32_array("tokenizer.ggml.scores",
                     {0.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f});
    w.meta_bool("tokenizer.ggml.add_bos_token", true);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 1);

    // GGML dims are {in, out}; GGUFFile reverses them to row-major
    // [out, in] on load (see SPEC §4 and gguf_loader_test).
    const std::vector<float> emb(kHidden * kVocab, 0.05f);
    const std::vector<float> proj(kHidden * kHidden, 0.05f);
    const std::vector<float> kv(kHidden * (kKVHeads * kHeadDim), 0.05f);
    const std::vector<float> ffn(kHidden * kInter, 0.05f);
    const std::vector<float> down(kInter * kHidden, 0.05f);
    const std::vector<float> ones(kHidden, 1.0f);

    w.tensor({"token_embd.weight", {kHidden, kVocab}, td::GgufType::kF32, f32_bytes(emb)});
    w.tensor({"output_norm.weight", {kHidden}, td::GgufType::kF32, f32_bytes(ones)});
    w.tensor({"blk.0.attn_norm.weight", {kHidden}, td::GgufType::kF32, f32_bytes(ones)});
    w.tensor({"blk.0.attn_q.weight", {kHidden, kHidden}, td::GgufType::kF32, f32_bytes(proj)});
    w.tensor(
        {"blk.0.attn_k.weight", {kHidden, kKVHeads * kHeadDim}, td::GgufType::kF32, f32_bytes(kv)});
    w.tensor(
        {"blk.0.attn_v.weight", {kHidden, kKVHeads * kHeadDim}, td::GgufType::kF32, f32_bytes(kv)});
    w.tensor({"blk.0.attn_output.weight", {kHidden, kHidden}, td::GgufType::kF32, f32_bytes(proj)});
    w.tensor({"blk.0.ffn_norm.weight", {kHidden}, td::GgufType::kF32, f32_bytes(ones)});
    w.tensor({"blk.0.ffn_gate.weight", {kHidden, kInter}, td::GgufType::kF32, f32_bytes(ffn)});
    w.tensor({"blk.0.ffn_up.weight", {kHidden, kInter}, td::GgufType::kF32, f32_bytes(ffn)});
    w.tensor({"blk.0.ffn_down.weight", {kInter, kHidden}, td::GgufType::kF32, f32_bytes(down)});

    if (qkv_bias) {
        const std::vector<float> q_bias(kHeads * kHeadDim, 0.01f);
        const std::vector<float> kv_bias(kKVHeads * kHeadDim, 0.01f);
        w.tensor({"blk.0.attn_q.bias", {kHeads * kHeadDim}, td::GgufType::kF32, f32_bytes(q_bias)});
        w.tensor(
            {"blk.0.attn_k.bias", {kKVHeads * kHeadDim}, td::GgufType::kF32, f32_bytes(kv_bias)});
        w.tensor(
            {"blk.0.attn_v.bias", {kKVHeads * kHeadDim}, td::GgufType::kF32, f32_bytes(kv_bias)});
    }
    if (qk_norm) {
        const std::vector<float> norm(kHeadDim, 1.0f);
        w.tensor({"blk.0.attn_q_norm.weight", {kHeadDim}, td::GgufType::kF32, f32_bytes(norm)});
        w.tensor({"blk.0.attn_k_norm.weight", {kHeadDim}, td::GgufType::kF32, f32_bytes(norm)});
    }
    return w;
}

} // namespace

int main(int argc, char** argv) {
    std::string out_path = "/tmp/mllm_tiny.gguf";
    std::string arch = "llama";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (std::strcmp(argv[i], "--arch") == 0 && i + 1 < argc) {
            arch = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: %s [-o out.gguf] [--arch llama|qwen2|qwen3]\n", argv[0]);
            return 0;
        }
    }
    if (arch != "llama" && arch != "qwen2" && arch != "qwen3") {
        std::fprintf(stderr, "error: unsupported --arch %s\n", arch.c_str());
        return 1;
    }

    auto writer = build_writer(arch);
    const std::vector<uint8_t> bytes = writer.build(32);
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    std::printf("wrote %s (%zu bytes, arch=%s)\n", out_path.c_str(), bytes.size(), arch.c_str());
    return 0;
}
