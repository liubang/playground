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

#include "cpp/pl/mllm/tokenizer/tokenizer.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <utility>

namespace pl::mllm {

namespace {

// Decode a single token piece into raw UTF-8 bytes, applying llama.cpp-style
// byte fallback: pieces of the form "<0xAB>" map to that single byte.
[[nodiscard]] bool parse_byte_fallback(std::string_view piece, uint8_t& out) {
    // Expected shape: "<0xAB>"
    if (piece.size() != 6 || piece[0] != '<' || piece[1] != '0' || piece[2] != 'x' ||
        piece[5] != '>') {
        return false;
    }
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    const int hi = hex(piece[3]);
    const int lo = hex(piece[4]);
    if (hi < 0 || lo < 0)
        return false;
    out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

// Pretokenizer rules for LLaMA BPE: split on whitespace, keep leading space
// attached to the following piece. This is a faithful, compact implementation;
// it covers the common real-world prompts used for parity verification.
[[nodiscard]] std::vector<std::string> pre_tokenize_llama(std::string_view text) {
    std::vector<std::string> pieces;
    size_t i = 0;
    const auto flush = [&](size_t begin, size_t end) {
        if (end > begin)
            pieces.emplace_back(text.substr(begin, end - begin));
    };
    size_t word_begin = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            // collapse consecutive spaces into the following word's prefix
            flush(word_begin, i);
            size_t sp = i;
            while (sp < text.size()) {
                const unsigned char s = static_cast<unsigned char>(text[sp]);
                if (s == ' ' || s == '\t' || s == '\n' || s == '\r') {
                    ++sp;
                } else {
                    break;
                }
            }
            // attach a single leading space + the rest of the word as one piece
            if (sp < text.size()) {
                size_t we = sp;
                while (we < text.size()) {
                    const unsigned char w = static_cast<unsigned char>(text[we]);
                    if (w == ' ' || w == '\t' || w == '\n' || w == '\r')
                        break;
                    ++we;
                }
                std::string piece;
                piece.push_back(' ');
                piece.append(text, sp, we - sp);
                pieces.push_back(std::move(piece));
                i = we;
                word_begin = we;
            } else {
                // trailing whitespace
                std::string piece;
                piece.push_back(static_cast<char>(text[i]));
                pieces.push_back(std::move(piece));
                i = sp;
                word_begin = sp;
            }
        } else {
            ++i;
        }
    }
    flush(word_begin, i);
    return pieces;
}

// UTF-8 to codepoint decoding (for byte fallback re-encoding is not needed; we
// keep raw bytes). Returns codepoint length in bytes.
int utf8_len(unsigned char c) {
    if ((c & 0x80) == 0x00)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1; // invalid; treat as 1 byte
}

// GPT-2 byte-to-unicode table: maps each byte (0..255) to a unicode character
// representation. Bytes 33-126, 161-172, 174-255 map to themselves (printable);
// the remaining bytes are remapped to U+0100+i, where i is the byte's index in
// the sorted list of missing bytes (0-32 -> 256+i, 127-160 -> 289+(b-127),
// 173 -> 323). This is the standard bytes_to_unicode() from the original GPT-2
// implementation. The byte 155 (0x9B) maps to U+0135, not U+019B — the
// naive "256 + byte" mapping is wrong and breaks CJK vocab lookup.
[[nodiscard]] int32_t gpt2_byte_to_codepoint(uint8_t b) noexcept {
    if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) {
        return b;
    }
    if (b <= 32) {
        return 256 + b;
    }
    if (b >= 127 && b <= 160) {
        return 289 + (b - 127);
    }
    // b == 173 is the 68th missing byte -> 256 + 67 = 323
    return 323;
}

[[nodiscard]] std::string gpt2_byte_to_unicode(uint8_t b) {
    const int32_t cp = gpt2_byte_to_codepoint(b);
    // Encode as UTF-8.
    if (cp < 0x80) {
        return std::string(1, static_cast<char>(cp));
    }
    if (cp < 0x800) {
        std::string out;
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return out;
    }
    std::string out;
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return out;
}

// Precompute the full byte->unicode mapping table once.
[[nodiscard]] const std::array<std::string, 256>& gpt2_byte_table() {
    static const std::array<std::string, 256> table = [] {
        std::array<std::string, 256> t;
        for (int i = 0; i < 256; ++i) {
            t[static_cast<size_t>(i)] = gpt2_byte_to_unicode(static_cast<uint8_t>(i));
        }
        return t;
    }();
    return table;
}

// Reverse mapping: unicode codepoint -> byte index. Used during decode to
// convert byte-level unicode pieces back to raw bytes.
[[nodiscard]] int gpt2_unicode_to_byte(int32_t cp) {
    // Self-mapped range: byte == codepoint
    if ((cp >= 33 && cp <= 126) || (cp >= 161 && cp <= 172) || (cp >= 174 && cp <= 255)) {
        return cp;
    }
    // Remapped missing bytes: 0-32 -> 256+b, 127-160 -> 289+(b-127), 173 -> 323.
    if (cp >= 256 && cp <= 288) {
        return cp - 256;
    }
    if (cp >= 289 && cp <= 322) {
        return 127 + (cp - 289);
    }
    if (cp == 323) {
        return 173;
    }
    return -1; // not a byte-level mapping
}

// Decode a UTF-8 codepoint from the beginning of `s`. Returns the codepoint and
// the number of bytes consumed.
[[nodiscard]] std::pair<int32_t, int> decode_utf8_cp(std::string_view s) {
    if (s.empty())
        return {-1, 0};
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if ((c & 0x80) == 0x00) {
        return {static_cast<int32_t>(c), 1};
    }
    if ((c & 0xE0) == 0xC0 && s.size() >= 2) {
        const int32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[1]) & 0x3F);
        return {cp, 2};
    }
    if ((c & 0xF0) == 0xE0 && s.size() >= 3) {
        const int32_t cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
                           (static_cast<unsigned char>(s[2]) & 0x3F);
        return {cp, 3};
    }
    if ((c & 0xF8) == 0xF0 && s.size() >= 4) {
        const int32_t cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 12) |
                           ((static_cast<unsigned char>(s[2]) & 0x3F) << 6) |
                           (static_cast<unsigned char>(s[3]) & 0x3F);
        return {cp, 4};
    }
    // Invalid UTF-8; treat the single byte as a codepoint.
    return {static_cast<int32_t>(c), 1};
}

// GPT-2 pre-tokenization regex (simplified but correct for common inputs):
// Split text into words by whitespace, keeping the leading space with the word,
// then further split on word boundaries. This is a faithful implementation of
// the GPT-2 / Qwen2 pre-tokenizer used by llama.cpp.
//
// The actual GPT-2 regex is:
//   's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
// Since we don't have a regex engine, we implement a simplified version that
// matches the behavior on ASCII and common CJK text: group runs of letters,
// runs of digits, and runs of punctuation, each prefixed by an optional space.
[[nodiscard]] std::vector<std::string> pre_tokenize_gpt2_impl(std::string_view text) {
    std::vector<std::string> pieces;
    size_t i = 0;

    while (i < text.size()) {
        // Consume optional leading space (at most one, per GPT-2 convention).
        bool has_leading_space = false;
        if (i < text.size() && text[i] == ' ') {
            has_leading_space = true;
            ++i;
        }

        if (i >= text.size()) {
            // Trailing space with nothing after it: emit as a piece.
            pieces.emplace_back(" ");
            break;
        }

        const unsigned char c = static_cast<unsigned char>(text[i]);

        // Determine the character class.
        // Letter: a-z, A-Z, and all multi-byte UTF-8 (treat CJK as letters).
        // Digit: 0-9.
        // Other: everything else (punctuation, etc.).
        auto char_class = [](unsigned char ch) -> int {
            if (ch >= '0' && ch <= '9')
                return 1; // digit
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
                return 0; // letter
            if (ch >= 0x80)
                return 0; // multi-byte UTF-8: treat as letter (CJK, etc.)
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
                return 2; // whitespace
            return 3;     // punctuation/other
        };

        const int cls = char_class(c);
        if (cls == 2) {
            // Whitespace after optional space: emit a whitespace piece.
            // This handles \s+ runs that aren't captured by leading space.
            size_t start = i - (has_leading_space ? 1 : 0);
            while (i < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[i]);
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
                    break;
                ++i;
            }
            pieces.emplace_back(text.substr(start, i - start));
            continue;
        }

        // Consume a run of the same class.
        size_t start = i - (has_leading_space ? 1 : 0);
        if (cls == 0) {
            // Letter run (including multi-byte UTF-8 consumed per-codepoint).
            while (i < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[i]);
                if (ch >= 0x80) {
                    // Consume the full multi-byte sequence.
                    int n = utf8_len(ch);
                    if (i + static_cast<size_t>(n) > text.size())
                        n = 1;
                    i += static_cast<size_t>(n);
                } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
                    ++i;
                } else {
                    break;
                }
            }
        } else if (cls == 1) {
            // Digit run.
            while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                ++i;
        } else {
            // Punctuation/other run: consume until we hit a letter, digit, or
            // whitespace.
            while (i < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[i]);
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
                    break;
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
                    break;
                if (ch >= '0' && ch <= '9')
                    break;
                if (ch >= 0x80) {
                    // Multi-byte: part of CJK etc. -> belongs to letter class,
                    // so punctuation run stops here.
                    break;
                }
                ++i;
            }
        }

        pieces.emplace_back(text.substr(start, i - start));
    }

    return pieces;
}

} // namespace

void Tokenizer::build_bytes_map() {
    byte_to_id_.assign(256, -1);
    for (size_t id = 0; id < tokens_.size(); ++id) {
        uint8_t b = 0;
        if (parse_byte_fallback(tokens_[id], b)) {
            byte_to_id_[b] = static_cast<int32_t>(id);
        }
    }
}

void Tokenizer::load_vocab(const GGUFFile& file, const std::vector<std::string>& tokens) {
    tokens_ = tokens;
    for (size_t id = 0; id < tokens_.size(); ++id) {
        piece_to_id_.emplace(tokens_[id], static_cast<int32_t>(id));
    }

    // BOS / EOS / add_bos from metadata (defaults match llama.cpp).
    if (auto b = file.u32_meta("tokenizer.ggml.bos_token_id"); b.ok()) {
        bos_id_ = static_cast<int32_t>(b.value());
    }
    if (auto e = file.u32_meta("tokenizer.ggml.eos_token_id"); e.ok()) {
        eos_id_ = static_cast<int32_t>(e.value());
    }
    if (auto a = file.bool_meta("tokenizer.ggml.add_bos_token"); a.ok()) {
        add_bos_ = a.value();
    }

    // Collect special tokens (anything that is not a plain NORMAL piece or a
    // byte-fallback piece). This covers CONTROL (<|im_start|>, <|im_end|>,
    // <|endoftext|>) and USER_DEFINED (Qwen3 <think>, </think>) tokens, which
    // must be matched as whole units during Encode so the BPE pass never
    // splits them. Sorted by text length descending so a greedy longest match
    // is correct. (llama.cpp's `llama_token_is_special` equivalent.)
    if (auto tt = file.i32_array_meta("tokenizer.ggml.token_type"); tt.ok()) {
        const auto& types = tt.value();
        const size_t n = std::min(tokens.size(), types.size());
        for (size_t i = 0; i < n; ++i) {
            // NORMAL=1, BYTE=6 are the only non-special types.
            if (types[i] != 1 && types[i] != 6) {
                special_tokens_.emplace_back(tokens[i], static_cast<int32_t>(i));
            }
        }
        std::sort(special_tokens_.begin(), special_tokens_.end(), [](const auto& a, const auto& b) {
            return a.first.size() > b.first.size();
        });
    }
    build_bytes_map();
}

void Tokenizer::build_rank_table(const std::vector<std::string>& tokens,
                                 std::span<const float> scores) {
    // llama.cpp BPE: merge priority = score order (higher score first), stored
    // as a pair->rank lookup. When scores are absent (count mismatch), fall
    // back to using token id ordering for stable, deterministic merges.
    const size_t n = tokens.size();
    std::vector<int32_t> rank(n);
    for (size_t i = 0; i < n; ++i) {
        rank[i] = static_cast<int32_t>(i);
    }
    if (scores.size() == n && n > 1) {
        std::stable_sort(rank.begin(), rank.end(), [&](int32_t a, int32_t b) {
            return scores[static_cast<size_t>(a)] > scores[static_cast<size_t>(b)];
        });
    }
    // Build pair -> rank. A pair (l, r) is mergeable if concatenating tokens[l]
    // + tokens[r] equals some token. We encode pair as uint64 for fast lookup.
    for (int32_t r = 0; r < static_cast<int32_t>(n); ++r) {
        const int32_t tid = rank[static_cast<size_t>(r)];
        const std::string& piece = tokens[static_cast<size_t>(tid)];
        // Find a split point k such that piece = left + right and both are ids.
        for (size_t k = 1; k + 1 <= piece.size(); ++k) {
            const auto lit = piece_to_id_.find(piece.substr(0, k));
            if (lit == piece_to_id_.end())
                continue;
            const auto rit = piece_to_id_.find(piece.substr(k));
            if (rit == piece_to_id_.end())
                continue;
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(lit->second)) << 32) |
                                 static_cast<uint64_t>(static_cast<uint32_t>(rit->second));
            merge_rank_.emplace(key, r);
        }
    }
}

void Tokenizer::build_gpt2_merge_table(const GGUFFile& file) {
    // GPT-2 BPE: merges come from the `tokenizer.ggml.merges` string array.
    // Each entry is "left right" (space-separated). The rank of each merge
    // is its position in the array (lower index = higher priority).
    // We store pair -> rank in merge_rank_, same as the llama path.
    auto merges = file.str_array_meta("tokenizer.ggml.merges");
    if (!merges.ok()) {
        // Some GPT-2 GGUFs store merges as `tokenizer.ggml.bpe_merges`; try
        // the alternative key before giving up.
        merges = file.str_array_meta("tokenizer.ggml.bpe_merges");
        if (!merges.ok()) {
            return; // no merges; BPE will just emit byte-level tokens
        }
    }

    const auto& merge_list = merges.value();
    for (int32_t rank = 0; rank < static_cast<int32_t>(merge_list.size()); ++rank) {
        const std::string& m = merge_list[static_cast<size_t>(rank)];
        // Split on the first space: "left right"
        const auto sp = m.find(' ');
        if (sp == std::string::npos || sp == 0 || sp == m.size() - 1) {
            continue; // malformed merge entry; skip
        }
        const std::string left = m.substr(0, sp);
        const std::string right = m.substr(sp + 1);

        const auto lit = piece_to_id_.find(left);
        if (lit == piece_to_id_.end())
            continue;
        const auto rit = piece_to_id_.find(right);
        if (rit == piece_to_id_.end())
            continue;

        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(lit->second)) << 32) |
                             static_cast<uint64_t>(static_cast<uint32_t>(rit->second));
        // Only store the first (best) rank for a pair.
        merge_rank_.emplace(key, rank);
    }
}

int32_t Tokenizer::id_for_piece(const std::string& piece) const {
    const auto it = piece_to_id_.find(piece);
    return it == piece_to_id_.end() ? -1 : it->second;
}

size_t Tokenizer::match_special(std::string_view text, size_t pos) const noexcept {
    for (const auto& [piece, id] : special_tokens_) {
        (void)id;
        if (piece.size() <= text.size() - pos && text.compare(pos, piece.size(), piece) == 0) {
            return piece.size();
        }
    }
    return 0;
}

std::vector<std::string> Tokenizer::pre_tokenize(std::string_view text) const {
    if (model_type_ == ModelType::kGpt2) {
        return pre_tokenize_gpt2(text);
    }
    return pre_tokenize_llama(text);
}

std::vector<std::string> Tokenizer::pre_tokenize_gpt2(std::string_view text) {
    return pre_tokenize_gpt2_impl(text);
}

std::vector<int32_t> Tokenizer::bpe_apply(const std::vector<std::string>& pieces) const {
    if (model_type_ == ModelType::kGpt2) {
        return bpe_merge_gpt2(pieces);
    }
    // llama BPE path (original implementation).
    std::vector<int32_t> out;
    out.reserve(pieces.size() * 2);
    for (const auto& word : pieces) {
        // Seed tokens with byte fallback (llama byte-level).
        std::vector<int32_t> toks;
        toks.reserve(word.size());
        for (size_t i = 0; i < word.size();) {
            const unsigned char b = static_cast<unsigned char>(word[i]);
            int n = utf8_len(b);
            if (i + static_cast<size_t>(n) > word.size()) {
                n = 1;
            }
            // Try the whole codepoint as a vocab piece first (greedy longest).
            bool matched = false;
            for (int k = n; k >= 1; --k) {
                const std::string sub = word.substr(i, static_cast<size_t>(k));
                const int32_t id = id_for_piece(sub);
                if (id >= 0) {
                    toks.push_back(id);
                    i += static_cast<size_t>(k);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                // byte fallback: emit one byte each via byte_to_id_ table.
                if (byte_to_id_[b] >= 0) {
                    toks.push_back(byte_to_id_[b]);
                }
                ++i;
            }
        }

        // Greedy BPE merges: repeatedly apply the best pair.
        bool changed = true;
        while (changed && toks.size() > 1) {
            changed = false;
            int best_rank = INT32_MAX;
            size_t best_pos = 0;
            int32_t best_id = -1;
            for (size_t p = 0; p + 1 < toks.size(); ++p) {
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(toks[p])) << 32) |
                                     static_cast<uint64_t>(static_cast<uint32_t>(toks[p + 1]));
                const auto it = merge_rank_.find(key);
                if (it == merge_rank_.end())
                    continue;
                if (it->second < best_rank) {
                    best_rank = it->second;
                    best_pos = p;
                    // reconstruct the merged token id: concatenation should be
                    // in vocab; find it lazily.
                    std::string merged;
                    merged.reserve(8);
                    // We do not have direct access to token strings for ids
                    // here without tokens_; keep a small lookup below.
                    best_id = -2; // sentinel: resolve after choosing best
                }
            }
            if (best_rank == INT32_MAX)
                break;
            // Resolve merged id by concatenating token text.
            std::string merged;
            merged.reserve(tokens_[static_cast<size_t>(toks[best_pos])].size() +
                           tokens_[static_cast<size_t>(toks[best_pos + 1])].size());
            merged += tokens_[static_cast<size_t>(toks[best_pos])];
            merged += tokens_[static_cast<size_t>(toks[best_pos + 1])];
            const int32_t merged_id = id_for_piece(merged);
            if (merged_id < 0) {
                break;
            }
            toks[best_pos] = merged_id;
            toks.erase(toks.begin() + static_cast<ptrdiff_t>(best_pos + 1));
            changed = true;
            (void)best_id;
        }
        out.insert(out.end(), toks.begin(), toks.end());
    }
    return out;
}

std::vector<int32_t> Tokenizer::bpe_merge_gpt2(const std::vector<std::string>& pieces) const {
    std::vector<int32_t> out;
    out.reserve(pieces.size() * 2);

    const auto& byte_table = gpt2_byte_table();

    for (const auto& word : pieces) {
        // Step 1: convert raw bytes of the word to the GPT-2 unicode
        // representation. Each byte -> its mapped unicode string. Then
        // concatenate to form the unicode-encoded piece sequence.
        //
        // We also track which token id each initial byte-mapped symbol
        // corresponds to (if it's in the vocab).
        std::vector<int32_t> toks;
        toks.reserve(word.size());

        // First, build the unicode-encoded string and seed the token list
        // with direct piece lookups.
        std::string encoded;
        encoded.reserve(word.size() * 2);
        for (size_t i = 0; i < word.size(); ++i) {
            const uint8_t b = static_cast<uint8_t>(word[i]);
            const std::string& mapped = byte_table[b];
            encoded += mapped;
        }

        // Try to match the longest vocab pieces greedily along the encoded
        // string. This handles the case where a multi-byte unicode piece is
        // directly in the vocab (e.g. CJK characters that are single tokens).
        for (size_t i = 0; i < encoded.size();) {
            bool matched = false;
            // Try longest match first (up to a reasonable limit).
            const size_t max_try = std::min<size_t>(encoded.size() - i, 16);
            for (size_t k = max_try; k >= 1; --k) {
                const std::string sub = encoded.substr(i, k);
                const int32_t id = id_for_piece(sub);
                if (id >= 0) {
                    toks.push_back(id);
                    i += k;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                // Fall back to single byte-mapped unicode character.
                // Find the single mapped char at position i.
                auto [cp, n] = decode_utf8_cp(std::string_view(encoded).substr(i));
                if (n > 0) {
                    // Try the single char as a piece.
                    const std::string single = encoded.substr(i, static_cast<size_t>(n));
                    const int32_t id = id_for_piece(single);
                    if (id >= 0) {
                        toks.push_back(id);
                    } else {
                        // Unknown: skip this codepoint (shouldn't happen in a
                        // well-formed gpt2 vocab, but be safe).
                        toks.push_back(-1);
                    }
                    i += static_cast<size_t>(n);
                } else {
                    ++i;
                }
            }
        }

        // Step 2: BPE merge pass — repeatedly apply the best (lowest-rank)
        // mergeable pair until no more merges are found.
        bool changed = true;
        while (changed && toks.size() > 1) {
            changed = false;
            int best_rank = INT32_MAX;
            size_t best_pos = 0;
            for (size_t p = 0; p + 1 < toks.size(); ++p) {
                if (toks[p] < 0 || toks[p + 1] < 0)
                    continue;
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(toks[p])) << 32) |
                                     static_cast<uint64_t>(static_cast<uint32_t>(toks[p + 1]));
                const auto it = merge_rank_.find(key);
                if (it == merge_rank_.end())
                    continue;
                if (it->second < best_rank) {
                    best_rank = it->second;
                    best_pos = p;
                }
            }
            if (best_rank == INT32_MAX)
                break;

            // Resolve merged id by concatenating token text.
            std::string merged;
            merged.reserve(tokens_[static_cast<size_t>(toks[best_pos])].size() +
                           tokens_[static_cast<size_t>(toks[best_pos + 1])].size());
            merged += tokens_[static_cast<size_t>(toks[best_pos])];
            merged += tokens_[static_cast<size_t>(toks[best_pos + 1])];
            const int32_t merged_id = id_for_piece(merged);
            if (merged_id < 0) {
                // This pair is in the merge table but the result isn't in the
                // vocab — shouldn't happen for well-formed GGUFs. Break to
                // avoid an infinite loop.
                break;
            }
            toks[best_pos] = merged_id;
            toks.erase(toks.begin() + static_cast<ptrdiff_t>(best_pos + 1));
            changed = true;
        }

        // Emit tokens, filtering out any -1 (unmappable) sentinels.
        for (int32_t t : toks) {
            if (t >= 0) {
                out.push_back(t);
            }
        }
    }
    return out;
}

std::string Tokenizer::gpt2_decode_piece(std::string_view piece) {
    // Reverse the GPT-2 byte-level unicode mapping: walk through the piece's
    // codepoints, map each back to a raw byte if possible, and concatenate.
    std::string out;
    out.reserve(piece.size());
    size_t i = 0;
    while (i < piece.size()) {
        auto [cp, n] = decode_utf8_cp(piece.substr(i));
        if (n == 0) {
            ++i;
            continue;
        }
        const int byte = gpt2_unicode_to_byte(cp);
        if (byte >= 0 && byte <= 255) {
            out.push_back(static_cast<char>(static_cast<uint8_t>(byte)));
        } else {
            // Not a byte-level codepoint; emit the raw UTF-8 bytes as-is.
            out.append(piece.substr(i, static_cast<size_t>(n)));
        }
        i += static_cast<size_t>(n);
    }
    return out;
}

Result<std::vector<int32_t>> Tokenizer::Encode(std::string_view text, bool add_bos) const {
    std::vector<int32_t> ids;
    if (add_bos && add_bos_) {
        ids.push_back(bos_id_);
    }

    // First split the text on special tokens (whole-unit matches). Ordinary
    // segments go through pre-tokenize + BPE; special tokens map directly to
    // their vocab id and are never split.
    const auto append_bpe = [&](std::string_view seg) {
        const auto body = bpe_apply(pre_tokenize(seg));
        ids.insert(ids.end(), body.begin(), body.end());
    };

    size_t i = 0;
    size_t seg_start = 0;
    while (i < text.size()) {
        const size_t sp_len = match_special(text, i);
        if (sp_len > 0) {
            if (i > seg_start) {
                append_bpe(text.substr(seg_start, i - seg_start));
            }
            const int32_t id = id_for_piece(std::string(text.substr(i, sp_len)));
            if (id >= 0) {
                ids.push_back(id);
            }
            i += sp_len;
            seg_start = i;
        } else {
            ++i;
        }
    }
    if (seg_start < text.size()) {
        append_bpe(text.substr(seg_start));
    }

    if (ids.empty() && !add_bos) {
        ids.push_back(eos_id_);
    }
    return ids;
}

Result<std::string> Tokenizer::DecodeOne(int32_t token) const {
    if (token < 0 || token >= static_cast<int32_t>(tokens_.size())) {
        return Status::Error(ErrorCode::kInvalidArgument, "token id out of range");
    }
    const std::string& piece = tokens_[static_cast<size_t>(token)];

    // llama byte-fallback: "<0xAB>" -> single raw byte.
    uint8_t b = 0;
    if (parse_byte_fallback(piece, b)) {
        return std::string(reinterpret_cast<const char*>(&b), 1);
    }

    // GPT-2 byte-level: reverse the unicode mapping.
    if (model_type_ == ModelType::kGpt2) {
        return gpt2_decode_piece(piece);
    }

    // The leading-space marker is a literal ' ' in llama BPE pieces; keep as-is.
    return piece;
}

Result<std::string> Tokenizer::Decode(std::span<const int32_t> tokens) const {
    std::string out;
    for (const int32_t t : tokens) {
        auto p = DecodeOne(t);
        if (!p.ok())
            return p.status();
        out += p.value();
    }
    // llama.cpp strips the first leading space after BOS; leave as-is for tests.
    return out;
}

Result<Tokenizer> Tokenizer::FromGGUF(const GGUFFile& file) {
    Tokenizer tok;
    auto tokens = file.str_array_meta("tokenizer.ggml.tokens");
    if (!tokens.ok()) {
        return Status::Error(ErrorCode::kInvalidFormat, "tokenizer.ggml.tokens missing");
    }
    const auto& vocab = tokens.value();
    tok.load_vocab(file, std::vector<std::string>(vocab.begin(), vocab.end()));

    // Detect tokenizer family: "gpt2" switches to byte-level BPE (Qwen2/Qwen3),
    // everything else falls through to the llama SentencePiece-style path.
    if (auto model = file.string_meta("tokenizer.ggml.model"); model.ok()) {
        if (model.value() == "gpt2") {
            tok.model_type_ = ModelType::kGpt2;
        }
    }

    if (tok.model_type_ == ModelType::kGpt2) {
        // GPT-2 path: build the merge table from tokenizer.ggml.merges.
        tok.build_gpt2_merge_table(file);
    } else {
        // LLaMA path: build the rank table from tokenizer.ggml.scores.
        std::vector<float> scores;
        if (auto s = file.f32_array_meta("tokenizer.ggml.scores"); s.ok()) {
            scores.assign(s.value().begin(), s.value().end());
        }
        tok.build_rank_table(std::vector<std::string>(vocab.begin(), vocab.end()),
                             std::span<const float>(scores));
    }
    return tok;
}

} // namespace pl::mllm
