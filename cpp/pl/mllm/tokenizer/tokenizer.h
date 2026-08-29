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

// LLaMA / SentencePiece-compatible byte-level BPE tokenizer built from GGUF
// tokenizer metadata. Matches llama.cpp's encode/decode semantics closely
// enough for token-id parity on real models.
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
    struct Merge {
        int32_t left = -1;
        int32_t right = -1;
    };

    void load_vocab(const GGUFFile& file, const std::vector<std::string>& tokens);
    void build_rank_table(const std::vector<std::string>& tokens, std::span<const float> scores);
    void build_bytes_map();

    [[nodiscard]] std::vector<std::string> pre_tokenize(std::string_view text) const;
    [[nodiscard]] std::vector<int32_t> bpe_apply(const std::vector<std::string>& pieces) const;
    [[nodiscard]] int32_t id_for_piece(const std::string& piece) const;

    std::vector<std::string> tokens_;
    std::vector<std::string> token_text_; // decoded text (byte fallback decoded)
    std::unordered_map<std::string, int32_t> piece_to_id_;
    std::unordered_map<uint64_t, int32_t> merge_rank_; // (left<<32 | right) -> rank
    int32_t bos_id_ = 1;
    int32_t eos_id_ = 2;
    bool add_bos_ = false;
    // byte -> token id for 256 possible byte values (llama byte fallback)
    std::vector<int32_t> byte_to_id_;
    // utf8 byte fallback mapping: token id -> printable single-byte char if needed
    std::string byte_token_text_[256];
};

} // namespace pl::mllm
