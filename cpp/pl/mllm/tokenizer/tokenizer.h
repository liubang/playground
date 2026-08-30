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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/loader/gguf.h"

namespace pl::mllm {

// BPE tokenizer built from GGUF tokenizer metadata. Supports both tokenizer
// families used by the model registry:
//   * llama (SentencePiece-style): byte-fallback (+ scores-ordered merges).
//   * gpt2 (GPT-2 byte-level BPE, used by Qwen2/Qwen3): bytes are mapped to
//     the printable unicode range before merging, merge priorities come from
//     tokenizer.ggml.merges, and pre-tokenization follows the GPT-2 pattern.
// Matches llama.cpp's encode/decode semantics closely enough for token-id
// parity on real models.
class Tokenizer {
public:
    [[nodiscard]] static Result<Tokenizer> FromGGUF(const GGUFFile& file);

    [[nodiscard]] int32_t bos_id() const noexcept { return bos_id_; }
    [[nodiscard]] int32_t eos_id() const noexcept { return eos_id_; }
    [[nodiscard]] int32_t vocab_size() const noexcept {
        return static_cast<int32_t>(tokens_.size());
    }

    // add_bos prepends BOS when the model requests it (tokenizer.ggml.add_bos_token).
    [[nodiscard]] Result<std::vector<int32_t>> Encode(std::string_view text, bool add_bos) const;
    [[nodiscard]] Result<std::string> Decode(std::span<const int32_t> tokens) const;
    [[nodiscard]] Result<std::string> DecodeOne(int32_t token) const;

private:
    // Tokenizer family as declared by tokenizer.ggml.model.
    enum class ModelType {
        kLlama, // SentencePiece-style byte fallback ("llama", default)
        kGpt2,  // GPT-2 byte-level BPE ("gpt2", used by Qwen2/Qwen3)
    };

    void load_vocab(const GGUFFile& file, const std::vector<std::string>& tokens);
    void build_rank_table(const std::vector<std::string>& tokens, std::span<const float> scores);
    void build_gpt2_merge_table(const GGUFFile& file);
    void build_bytes_map();

    [[nodiscard]] std::vector<std::string> pre_tokenize(std::string_view text) const;
    [[nodiscard]] static std::vector<std::string> pre_tokenize_gpt2(std::string_view text);
    [[nodiscard]] std::vector<int32_t> bpe_apply(const std::vector<std::string>& pieces) const;
    [[nodiscard]] std::vector<int32_t> bpe_merge_gpt2(const std::vector<std::string>& pieces) const;
    [[nodiscard]] int32_t id_for_piece(const std::string& piece) const;

    // Special GGUF tokens (any type other than NORMAL or BYTE: CONTROL such
    // as <|im_start|>, <|im_end|>, <|endoftext|>, plus USER_DEFINED such as
    // Qwen3 <think>, </think>). They are matched as whole units during Encode
    // so the BPE pass never splits them.
    [[nodiscard]] size_t match_special(std::string_view text, size_t pos) const noexcept;

    [[nodiscard]] static std::string gpt2_decode_piece(std::string_view piece);

    std::vector<std::string> tokens_;
    std::unordered_map<std::string, int32_t> piece_to_id_;
    std::unordered_map<uint64_t, int32_t> merge_rank_; // (left<<32 | right) -> rank
    // (text, id) pairs of GGUF special tokens, sorted by text length
    // descending so a greedy longest match during Encode is correct.
    std::vector<std::pair<std::string, int32_t>> special_tokens_;
    int32_t bos_id_ = 1;
    int32_t eos_id_ = 2;
    bool add_bos_ = false;
    ModelType model_type_ = ModelType::kLlama;
    // byte -> token id for 256 possible byte values (llama byte fallback)
    std::vector<int32_t> byte_to_id_;
};

} // namespace pl::mllm
