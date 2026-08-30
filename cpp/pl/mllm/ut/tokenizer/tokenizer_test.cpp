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

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cpp/pl/mllm/loader/gguf.h"
#include "cpp/pl/mllm/tokenizer/tokenizer.h"
#include "cpp/pl/mllm/ut/testdata/gguf_writer.h"
#include "gtest/gtest.h"

namespace pl::mllm {
namespace {

namespace td = pl::mllm::testdata;

class TempFile {
public:
    explicit TempFile(std::vector<uint8_t> bytes)
        : path_(std::filesystem::temp_directory_path() / "mllm_tok_test_XXXXXX") {
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

// ---------------------------------------------------------------------------
// GPT-2 byte-level BPE test fixtures
// ---------------------------------------------------------------------------

// Minimal GPT-2 vocab covering "hello world" plus a space byte. Pieces are
// the GPT-2 byte-level unicode representation (printable ASCII maps to
// itself; space is remapped to U+0120 = UTF-8 "\xC4\xA0"). Merges exercise
// both direct vocab lookup ("hello") and the merge path ("world").
td::GgufWriter make_gpt2_writer() {
    td::GgufWriter w("qwen2");
    w.meta_string("tokenizer.ggml.model", "gpt2");

    const std::string sp = "\xC4\xA0"; // GPT-2 byte-level encoding of space

    w.meta_str_array("tokenizer.ggml.tokens",
                     {"h",
                      "e",
                      "l",
                      "o",
                      "he",
                      "ll",
                      "lo",
                      "hello",
                      "w",
                      "r",
                      "d",
                      "wo",
                      "rl",
                      "rld",
                      "world",
                      sp,
                      "<|endoftext|>"});
    w.meta_str_array("tokenizer.ggml.merges",
                     {
                         "h e",   // 0: h(0)+e(1) -> he(4)
                         "l l",   // 1: l(2)+l(2) -> ll(5)
                         "l o",   // 2: l(2)+o(3) -> lo(6)
                         "w o",   // 3: w(8)+o(3) -> wo(11)
                         "r l",   // 4: r(9)+l(2) -> rl(12)
                         "rl d",  // 5: rl(12)+d(10) -> rld(13)
                         "wo rld" // 6: wo(11)+rld(13) -> world(14)
                     });
    w.meta_bool("tokenizer.ggml.add_bos_token", false);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 16);

    w.meta_u32("qwen2.context_length", 32);
    w.meta_u32("qwen2.embedding_length", 8);
    w.meta_u32("qwen2.feed_forward_length", 16);
    w.meta_u32("qwen2.block_count", 1);
    w.meta_u32("qwen2.attention.head_count", 2);
    w.meta_u32("qwen2.attention.head_count_kv", 1);
    w.meta_f32("qwen2.attention.layer_norm_rms_epsilon", 1e-5f);

    std::vector<uint8_t> emb(8 * 17 * 4, 0); // [8, 17] ggml order
    w.tensor({"token_embd.weight", {8, 17}, td::GgufType::kF32, emb});

    return w;
}

TEST(TokenizerTest, Gpt2EncodeHello) {
    auto w = make_gpt2_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // "hello" -> all printable ASCII, encoded as "hello" in byte-level.
    // Longest match should find "hello" (id=7) directly.
    auto ids = tok.value().Encode("hello", false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 1u);
    EXPECT_EQ(ids.value()[0], 7);
}

TEST(TokenizerTest, Gpt2EncodeWorld) {
    auto w = make_gpt2_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // "world" -> byte-level encoded as "world".
    // Longest match: "wo"(11) + "rl"(12) + "d"(10).
    // BPE merge[3]: "w o" -> "wo" (rank 3)
    // BPE merge[4]: "r l" -> "rl" (rank 4)
    // BPE merge[5]: "rl d" -> "rld" (rank 5)
    // BPE merge[6]: "wo rld" -> "world" (rank 6)
    // After merges: should be [14] = "world".
    auto ids = tok.value().Encode("world", false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    // After BPE merges, "world" should be a single token (id=14).
    ASSERT_EQ(ids.value().size(), 1u);
    EXPECT_EQ(ids.value()[0], 14);
}

TEST(TokenizerTest, Gpt2EncodeHelloWorld) {
    auto w = make_gpt2_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // "hello world" -> pre-tokenized as ["hello", " world"]
    // "hello" -> [7] (direct match)
    // " world" -> byte-level encoded as sp + "world"
    //   sp = "\xC4\xA0" (id=15), "world" -> [14] after BPE
    // Result: [7, 15, 14]
    auto ids = tok.value().Encode("hello world", false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 3u);
    EXPECT_EQ(ids.value()[0], 7);  // "hello"
    EXPECT_EQ(ids.value()[1], 15); // sp (byte-level space)
    EXPECT_EQ(ids.value()[2], 14); // "world"
}

TEST(TokenizerTest, Gpt2DecodeRoundTrip) {
    auto w = make_gpt2_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    const std::string input = "hello world";
    auto ids = tok.value().Encode(input, false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    auto decoded = tok.value().Decode(std::span<const int32_t>(ids.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message;
    EXPECT_EQ(decoded.value(), input);
}

TEST(TokenizerTest, Gpt2DecodeSingleToken) {
    auto w = make_gpt2_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // Token 15 = sp = "\xC4\xA0" -> should decode to " "
    auto s = tok.value().DecodeOne(15);
    ASSERT_TRUE(s.ok()) << s.status().message;
    EXPECT_EQ(s.value(), " ");

    // Token 7 = "hello" -> should decode to "hello"
    s = tok.value().DecodeOne(7);
    ASSERT_TRUE(s.ok()) << s.status().message;
    EXPECT_EQ(s.value(), "hello");
}

TEST(TokenizerTest, Gpt2VocabSize) {
    auto w = make_gpt2_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;
    EXPECT_EQ(tok.value().vocab_size(), 17);
    EXPECT_EQ(tok.value().eos_id(), 16);
}

// Special tokens (CONTROL / USER_DEFINED) must be matched as whole units and
// never split by the BPE pass. This mirrors Qwen3's <think> (USER_DEFINED)
// and <|im_start|> (CONTROL) handling.
td::GgufWriter make_special_tokens_writer() {
    td::GgufWriter w("qwen2");
    w.meta_string("tokenizer.ggml.model", "gpt2");

    const std::string sp = "\xC4\xA0"; // GPT-2 byte-level encoding of space

    w.meta_str_array("tokenizer.ggml.tokens", {"h",
                                               "e",
                                               "l",
                                               "o",
                                               "he",
                                               "ll",
                                               "lo",
                                               "hello",
                                               "w",
                                               "r",
                                               "d",
                                               "wo",
                                               "rl",
                                               "rld",
                                               "world",
                                               sp,
                                               "<|endoftext|>",
                                               "<|im_start|>",
                                               "<|im_end|>",
                                               "<think>",
                                               "</think>"});
    // token types: 1 = NORMAL for all word pieces, 3 = CONTROL for the
    // <|...|> tokens, 4 = USER_DEFINED for <think>/</think> (as in Qwen3).
    w.meta_i32_array("tokenizer.ggml.token_type",
                     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 4, 4});
    w.meta_str_array("tokenizer.ggml.merges",
                     {"h e", "l l", "l o", "w o", "r l", "rl d", "wo rld"});
    w.meta_bool("tokenizer.ggml.add_bos_token", false);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 16);

    w.meta_u32("qwen2.context_length", 32);
    w.meta_u32("qwen2.embedding_length", 8);
    w.meta_u32("qwen2.feed_forward_length", 16);
    w.meta_u32("qwen2.block_count", 1);
    w.meta_u32("qwen2.attention.head_count", 2);
    w.meta_u32("qwen2.attention.head_count_kv", 1);
    w.meta_f32("qwen2.attention.layer_norm_rms_epsilon", 1e-5f);

    std::vector<uint8_t> emb(8 * 21 * 4, 0); // [8, 21] ggml order
    w.tensor({"token_embd.weight", {8, 21}, td::GgufType::kF32, emb});

    return w;
}

TEST(TokenizerTest, Gpt2SpecialTokensNotSplit) {
    auto w = make_special_tokens_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // <think> is USER_DEFINED (id=19): must stay one token, not split into
    // '<'(28) + 'think'(...) + '>'.
    auto ids = tok.value().Encode("hello<think>", false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 2u);
    EXPECT_EQ(ids.value()[0], 7);  // "hello"
    EXPECT_EQ(ids.value()[1], 19); // "<think>"

    // <|im_start|> is CONTROL (id=17).
    ids = tok.value().Encode("<|im_start|>hello", false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 2u);
    EXPECT_EQ(ids.value()[0], 17); // "<|im_start|>"
    EXPECT_EQ(ids.value()[1], 7);  // "hello"

    // Mixed chat-style template with both kinds of special tokens.
    ids = tok.value().Encode("<|im_start|>hello<|im_end|>", false);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 3u);
    EXPECT_EQ(ids.value()[0], 17); // "<|im_start|>"
    EXPECT_EQ(ids.value()[1], 7);  // "hello"
    EXPECT_EQ(ids.value()[2], 18); // "<|im_end|>"
}

// ---------------------------------------------------------------------------
// LLaMA BPE test fixtures
// ---------------------------------------------------------------------------

// Build a minimal LLaMA-style tokenizer with byte fallback.
// Vocab:
//   0: "<s>"           (BOS)
//   1: "</s>"          (EOS)
//   2: "a"
//   3: "b"
//   4: "c"
//   5: "ab"             (merge of "a"+"b")
//   6: " "              (space, as a token piece)
//   7: " a"             (space + a)
//   8: "<0x80>"         (byte fallback for 0x80)
//
// Scores: higher score = higher merge priority.
// scores[5] (for "ab") = 1.0 (highest)
// scores[7] (for " a") = 0.5
// scores[2..4] = -1.0, 0.0, 0.5 (lower priority)

td::GgufWriter make_llama_writer() {
    td::GgufWriter w("llama");
    // No tokenizer.ggml.model -> defaults to kLlama path.
    w.meta_str_array("tokenizer.ggml.tokens",
                     {"<s>", "</s>", "a", "b", "c", "ab", " ", " a", "<0x80>"});
    w.meta_f32_array("tokenizer.ggml.scores",
                     {0.0f, 0.0f, -1.0f, 0.0f, 0.5f, 1.0f, -0.5f, 0.5f, -2.0f});
    w.meta_bool("tokenizer.ggml.add_bos_token", true);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 1);

    w.meta_u32("llama.context_length", 32);
    w.meta_u32("llama.embedding_length", 8);
    w.meta_u32("llama.feed_forward_length", 16);
    w.meta_u32("llama.block_count", 1);
    w.meta_u32("llama.attention.head_count", 2);
    w.meta_u32("llama.attention.head_count_kv", 1);
    w.meta_f32("llama.attention.layer_norm_rms_epsilon", 1e-5f);

    std::vector<uint8_t> emb(8 * 9 * 4, 0); // [8, 9] ggml order
    w.tensor({"token_embd.weight", {8, 9}, td::GgufType::kF32, emb});

    return w;
}

TEST(TokenizerTest, LlamaEncodeSimpleWord) {
    auto w = make_llama_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // "ab" -> pre-tokenized as ["ab"], BPE should merge "a"+"b" -> "ab"(5)
    auto ids = tok.value().Encode("ab", true);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    // BOS(0) + "ab"(5)
    ASSERT_EQ(ids.value().size(), 2u);
    EXPECT_EQ(ids.value()[0], 0); // BOS
    EXPECT_EQ(ids.value()[1], 5); // "ab"
}

TEST(TokenizerTest, LlamaEncodeWithSpace) {
    auto w = make_llama_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // " ab" -> pre-tokenized as [" ab"]
    // Seed: " "(6) + "a"(2) + "b"(3)
    // BPE merge[best]: "a"+"b" -> "ab"(5) (score 1.0 > score 0.5 for " a")
    // Then " "+"ab" -> " ab" not in vocab, merge stops.
    // Result: BOS(0) + " "(6) + "ab"(5)
    auto ids = tok.value().Encode(" ab", true);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 3u);
    EXPECT_EQ(ids.value()[0], 0); // BOS
    EXPECT_EQ(ids.value()[1], 6); // " "
    EXPECT_EQ(ids.value()[2], 5); // "ab"
}

TEST(TokenizerTest, LlamaDecodeByteFallback) {
    auto w = make_llama_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // Token 8 = "<0x80>" -> should decode to byte 0x80
    auto s = tok.value().DecodeOne(8);
    ASSERT_TRUE(s.ok()) << s.status().message;
    EXPECT_EQ(s.value(), std::string(1, '\x80'));
}

TEST(TokenizerTest, LlamaVocabSize) {
    auto w = make_llama_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;
    EXPECT_EQ(tok.value().vocab_size(), 9);
    EXPECT_EQ(tok.value().bos_id(), 0);
    EXPECT_EQ(tok.value().eos_id(), 1);
}

TEST(TokenizerTest, LlamaDecodeRoundTrip) {
    auto w = make_llama_writer();
    TempFile tmp(w.build(32));
    auto file = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(file.ok()) << file.status().message;
    auto tok = Tokenizer::FromGGUF(*file.value());
    ASSERT_TRUE(tok.ok()) << tok.status().message;

    // Encode "ab" (with BOS), then decode (skip BOS).
    auto ids = tok.value().Encode("ab", true);
    ASSERT_TRUE(ids.ok()) << ids.status().message;
    ASSERT_EQ(ids.value().size(), 2u);
    // Decode token 5 ("ab") should give "ab".
    auto decoded = tok.value().DecodeOne(5);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message;
    EXPECT_EQ(decoded.value(), "ab");
}

} // namespace
} // namespace pl::mllm
