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

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/loader/gguf.h"
#include "cpp/pl/mllm/model/config.h"
#include "cpp/pl/mllm/ut/testdata/gguf_writer.h"
#include "gtest/gtest.h"

namespace pl::mllm {
namespace {

namespace td = pl::mllm::testdata;

class TempFile {
public:
    explicit TempFile(std::vector<uint8_t> bytes)
        : path_(std::filesystem::temp_directory_path() / ("mllm_test_XXXXXX")) {
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

td::GgufWriter make_minimal_writer() {
    td::GgufWriter w("llama");
    w.meta_u32("llama.context_length", 64);
    w.meta_u32("llama.embedding_length", 16);
    w.meta_u32("llama.feed_forward_length", 32);
    w.meta_u32("llama.block_count", 1);
    w.meta_u32("llama.attention.head_count", 2);
    w.meta_u32("llama.attention.head_count_kv", 1);
    w.meta_f32("llama.attention.layer_norm_rms_epsilon", 1e-5f);
    w.meta_f32("llama.rope.freq_base", 10000.0f);
    return w;
}

std::vector<uint8_t> f16_bytes_from_floats(const std::vector<float>& values) {
    std::vector<uint8_t> out;
    out.reserve(values.size() * 2);
    for (float v : values) {
        const uint16_t h = fp32_to_fp16(v);
        out.push_back(static_cast<uint8_t>(h & 0xFF));
        out.push_back(static_cast<uint8_t>(h >> 8));
    }
    return out;
}

std::vector<uint8_t> f32_bytes_from_floats(const std::vector<float>& values) {
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

TEST(GgufLoaderTest, ParsesHeaderAndMetadata) {
    auto w = make_minimal_writer();
    w.meta_string("general.name", "tiny");
    w.meta_str_array("tokenizer.ggml.tokens", {"a", "b", "c"});
    w.meta_f32_array("tokenizer.ggml.scores", {0.1f, 0.2f, 0.3f});
    w.meta_bool("tokenizer.ggml.add_bos_token", true);

    // token_embd.weight: [embedding, vocab] in ggml order -> shape {vocab, emb}
    td::GgufTensorSpec emb{
        "token_embd.weight",
        {16, 3},
        td::GgufType::kF16,
        f16_bytes_from_floats({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f,
                               1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f,
                               2.1f, 2.2f, 2.3f, 2.4f, 2.5f, 2.6f, 2.7f, 2.8f, 2.9f, 3.0f,
                               3.1f, 3.2f, 3.3f, 3.4f, 3.5f, 3.6f, 3.7f, 3.8f, 3.9f, 4.0f,
                               4.1f, 4.2f, 4.3f, 4.4f, 4.5f, 4.6f, 4.7f, 4.8f})};
    w.tensor(std::move(emb));

    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    EXPECT_EQ(file.value()->architecture(), "llama");

    auto name = file.value()->string_meta("general.name");
    ASSERT_TRUE(name.ok());
    EXPECT_EQ(name.value(), "tiny");

    auto tok = file.value()->str_array_meta("tokenizer.ggml.tokens");
    ASSERT_TRUE(tok.ok());
    ASSERT_EQ(tok.value().size(), 3u);
    EXPECT_EQ(tok.value()[2], "c");

    auto bos = file.value()->bool_meta("tokenizer.ggml.add_bos_token");
    ASSERT_TRUE(bos.ok());
    EXPECT_TRUE(bos.value());
}

TEST(GgufLoaderTest, TensorDirectoryAndShape) {
    auto w = make_minimal_writer();
    // [16, 3] ggml -> row-major shape {3, 16}
    w.tensor(
        {"token_embd.weight", {16, 3}, td::GgufType::kF16, std::vector<uint8_t>(16 * 3 * 2, 0)});
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;

    const auto tensors = file.value()->tensors();
    ASSERT_EQ(tensors.size(), 1u);
    EXPECT_EQ(tensors[0].name, "token_embd.weight");
    EXPECT_EQ(tensors[0].shape, Shape({3, 16}));
    EXPECT_EQ(tensors[0].dtype, DType::kF16);
    EXPECT_EQ(tensors[0].byte_size, 16u * 3u * 2u);

    auto view = file.value()->tensor("token_embd.weight");
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().shape(), Shape({3, 16}));
    EXPECT_EQ(view.value().dtype(), DType::kF16);
}

TEST(GgufLoaderTest, ModelConfigFromMetadata) {
    auto w = make_minimal_writer();
    w.tensor(
        {"token_embd.weight", {16, 7}, td::GgufType::kF16, std::vector<uint8_t>(16 * 7 * 2, 0)});
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok());
    auto cfg = file.value()->model_config();
    ASSERT_TRUE(cfg.ok()) << cfg.status().message;
    EXPECT_EQ(cfg.value().architecture, "llama");
    EXPECT_EQ(cfg.value().hidden_size, 16);
    EXPECT_EQ(cfg.value().num_attention_heads, 2);
    EXPECT_EQ(cfg.value().num_kv_heads, 1);
    EXPECT_EQ(cfg.value().num_layers, 1);
    EXPECT_EQ(cfg.value().intermediate_size, 32);
    EXPECT_EQ(cfg.value().context_length, 64);
    EXPECT_EQ(cfg.value().vocab_size, 7);
    EXPECT_FLOAT_EQ(cfg.value().rms_norm_eps, 1e-5f);
    EXPECT_FLOAT_EQ(cfg.value().rope_freq_base, 10000.0f);
}

TEST(GgufLoaderTest, RejectsBadMagic) {
    std::vector<uint8_t> bytes{0x00, 0x01, 0x02, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    TempFile tmp(std::move(bytes));
    auto file = GGUFFile::Open(tmp.path());
    EXPECT_FALSE(file.ok());
    EXPECT_EQ(file.status().code, ErrorCode::kInvalidFormat);
}

TEST(GgufLoaderTest, RejectsTruncated) {
    auto w = make_minimal_writer();
    auto full = w.build(32);
    full.resize(full.size() / 2); // cut in half mid-tensor
    TempFile tmp(std::move(full));
    auto file = GGUFFile::Open(tmp.path());
    EXPECT_FALSE(file.ok());
}

TEST(GgufLoaderTest, RejectsUnsupportedDtype) {
    auto w = make_minimal_writer();
    // Q4_1 (type id 3) is not supported by the MVP.
    w.tensor(
        {"token_embd.weight", {16, 3}, static_cast<td::GgufType>(3), std::vector<uint8_t>(1, 0)});
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    EXPECT_FALSE(file.ok());
    EXPECT_EQ(file.status().code, ErrorCode::kUnsupported);
}

TEST(GgufLoaderTest, RejectsMissingFile) {
    auto file = GGUFFile::Open("/nonexistent/mllm_does_not_exist.gguf");
    EXPECT_FALSE(file.ok());
    EXPECT_EQ(file.status().code, ErrorCode::kNotFound);
}

TEST(GgufLoaderTest, F32TensorRoundTrip) {
    auto w = make_minimal_writer();
    const std::vector<float> data{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    w.tensor({"blk.0.attn_norm.weight", {6}, td::GgufType::kF32, f32_bytes_from_floats(data)});
    TempFile tmp(w.build(8));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok());
    auto v = file.value()->tensor("blk.0.attn_norm.weight");
    ASSERT_TRUE(v.ok());
    const auto span = v.value().span_as<const float>();
    ASSERT_EQ(span.size(), 6u);
    EXPECT_FLOAT_EQ(span[5], 6.0f);
}

// Regression: GGUF tensor `offset` fields are relative to the start of the
// data section, which begins only AFTER the whole tensor directory. Each tensor
// must resolve to its own bytes; a loader that computes the data base mid-loop
// (per-tensor) misplaces every tensor after the first by the remaining
// directory length (see real-model debug: TinyLlama 1.1B Q8_0).
TEST(GgufLoaderTest, MultiTensorDataOffsets) {
    auto w = make_minimal_writer();
    w.tensor({"t0", {4}, td::GgufType::kF32, f32_bytes_from_floats({1.0f, 2.0f, 3.0f, 4.0f})});
    w.tensor({"t1", {4}, td::GgufType::kF32, f32_bytes_from_floats({10.0f, 11.0f, 12.0f, 13.0f})});
    w.tensor({"t2", {4}, td::GgufType::kF32, f32_bytes_from_floats({20.0f, 21.0f, 22.0f, 23.0f})});
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok());

    const auto& ts = file.value()->tensors();
    ASSERT_EQ(ts.size(), 3u);

    // Offsets must be strictly increasing and non-overlapping.
    EXPECT_LT(ts[0].file_offset, ts[1].file_offset);
    EXPECT_GE(ts[1].file_offset, ts[0].file_offset + ts[0].byte_size);
    EXPECT_GE(ts[2].file_offset, ts[1].file_offset + ts[1].byte_size);

    // And each tensor must read back exactly the values it was written with.
    struct Case {
        const char* name;
        std::vector<float> expect;
    };
    for (const Case& c : std::vector<Case>{
             {"t0", {1, 2, 3, 4}}, {"t1", {10, 11, 12, 13}}, {"t2", {20, 21, 22, 23}}}) {
        auto v = file.value()->tensor(c.name);
        ASSERT_TRUE(v.ok()) << c.name;
        const auto span = v.value().span_as<const float>();
        ASSERT_EQ(span.size(), c.expect.size());
        for (size_t i = 0; i < span.size(); ++i) {
            EXPECT_FLOAT_EQ(span[i], c.expect[i]) << c.name << "[" << i << "]";
        }
    }
}

} // namespace
} // namespace pl::mllm
