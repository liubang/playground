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
// Created: 2026/06/04 13:06

#include "cpp/pl/sstv2/compress/multi_prefix.h"

#include <algorithm>
#include <unordered_map>

#include "absl/status/status.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::compress {

using encode::Varints;

MultiPrefixCompressor::MultiPrefixCompressor(Config config) : config_(config) {}

namespace {

// Compute LCP length between two string_views
size_t lcp_length(std::string_view a, std::string_view b) {
    size_t len = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < len && a[i] == b[i]) {
        ++i;
    }
    return i;
}

// Append a varint-encoded uint32 to a string
void append_varint(std::string& out, uint32_t value) {
    std::byte buf[Varints::kMaxVarint32Bytes];
    size_t n = Varints::encode_uint32(value, buf);
    out.append(reinterpret_cast<const char*>(buf), n);
}

// Read a varint from a byte span, advance offset
uint32_t read_varint(std::span<const std::byte> data, size_t& offset) {
    auto [value, consumed] = Varints::decode_uint32(data.subspan(offset));
    offset += consumed;
    return value;
}

} // namespace

absl::StatusOr<MultiPrefixCompressor::CompressResult> MultiPrefixCompressor::compress(
    std::span<const std::string_view> sorted_strings) {
    if (sorted_strings.empty()) {
        return CompressResult{"", {}, 0};
    }

    // Single-round implementation for correctness
    const size_t n = sorted_strings.size();

    // Step 1: Compute LCP between adjacent pairs and collect prefix candidates
    // A prefix candidate is a common prefix shared by a contiguous group of strings
    struct PrefixCandidate {
        std::string prefix;
        size_t count = 0; // number of strings this prefix covers
    };

    std::unordered_map<std::string, size_t> prefix_counts;

    for (size_t i = 0; i + 1 < n; ++i) {
        size_t lcp = lcp_length(sorted_strings[i], sorted_strings[i + 1]);
        if (lcp >= config_.min_prefix_len) {
            // Use the LCP as a prefix candidate
            std::string prefix(sorted_strings[i].substr(0, lcp));
            prefix_counts[prefix]++;
        }
    }

    // Each adjacency pair contributes 1 to the count.
    // A prefix covering K strings will have K-1 adjacency hits.
    // Actual coverage = count + 1.

    // Also consider truncations of long prefixes (strings might share shorter prefixes too)
    // For simplicity, we just use the exact LCP prefixes found above.

    // Convert to vector and sort by coverage (descending)
    std::vector<PrefixCandidate> candidates;
    candidates.reserve(prefix_counts.size());
    for (auto& [prefix, adj_count] : prefix_counts) {
        candidates.push_back({prefix, adj_count + 1});
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const PrefixCandidate& a, const PrefixCandidate& b) {
                  // Sort by savings: prefix_len * count (descending)
                  return a.prefix.size() * a.count > b.prefix.size() * b.count;
              });

    // Select top prefixes (up to 256 per round, but prefix_id is varint so no hard limit)
    // We use at most 255 prefixes (1-based IDs, 0 = no prefix)
    const size_t max_prefixes = std::min<size_t>(candidates.size(), 255);

    // Greedy selection: pick prefixes that actually match strings
    // For correctness, we need to verify each prefix actually applies to strings
    std::vector<std::string> selected_prefixes;
    selected_prefixes.reserve(max_prefixes);
    for (size_t i = 0; i < max_prefixes; ++i) {
        selected_prefixes.push_back(std::move(candidates[i].prefix));
    }

    // Sort selected prefixes by length descending for greedy longest-match
    std::sort(selected_prefixes.begin(),
              selected_prefixes.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

    // Step 2: Encode each string
    // Format per string: [prefix_id: varint] [suffix_length: varint] [suffix_bytes]
    // prefix_id: 0 = no prefix, 1..N = index into selected_prefixes (1-based)
    std::string encoded;
    size_t total_input_size = 0;
    for (const auto& sv : sorted_strings) {
        total_input_size += sv.size();
    }
    encoded.reserve(total_input_size);

    for (const auto& sv : sorted_strings) {
        // Find longest matching prefix
        uint32_t best_id = 0;
        size_t best_len = 0;
        for (size_t i = 0; i < selected_prefixes.size(); ++i) {
            const auto& prefix = selected_prefixes[i];
            if (prefix.size() <= sv.size() && prefix.size() > best_len &&
                sv.substr(0, prefix.size()) == prefix) {
                best_id = static_cast<uint32_t>(i + 1); // 1-based
                best_len = prefix.size();
                break; // already sorted by length desc, first match is longest
            }
        }

        std::string_view suffix = sv.substr(best_len);
        append_varint(encoded, best_id);
        append_varint(encoded, static_cast<uint32_t>(suffix.size()));
        encoded.append(suffix);
    }

    // Check compression ratio
    double ratio =
        1.0 - (static_cast<double>(encoded.size()) / static_cast<double>(total_input_size));

    CompressResult result;
    if (total_input_size == 0 || ratio < config_.min_compression_ratio) {
        // Compression not worthwhile, store raw with prefix_id=0 for all
        result.compressed_data.clear();
        result.prefix_directory.clear();
        result.num_rounds = 0;

        // Encode all strings with no prefix
        for (const auto& sv : sorted_strings) {
            append_varint(result.compressed_data, 0);
            append_varint(result.compressed_data, static_cast<uint32_t>(sv.size()));
            result.compressed_data.append(sv);
        }
    } else {
        result.compressed_data = std::move(encoded);
        result.prefix_directory = std::move(selected_prefixes);
        result.num_rounds = 1;
    }

    return result;
}

absl::StatusOr<std::string> MultiPrefixCompressor::decompress_one(
    std::span<const std::byte> compressed_data,
    const std::vector<std::string>& prefix_directory,
    size_t num_strings,
    size_t idx) {
    if (idx >= num_strings) {
        return absl::InvalidArgumentError("index out of range");
    }

    size_t offset = 0;
    for (size_t i = 0; i <= idx; ++i) {
        if (offset >= compressed_data.size()) {
            return absl::InternalError("compressed data truncated");
        }
        uint32_t prefix_id = read_varint(compressed_data, offset);
        uint32_t suffix_len = read_varint(compressed_data, offset);

        if (offset + suffix_len > compressed_data.size()) {
            return absl::InternalError("compressed data truncated");
        }

        if (i == idx) {
            std::string result;
            if (prefix_id > 0 && prefix_id <= prefix_directory.size()) {
                result = prefix_directory[prefix_id - 1];
            } else if (prefix_id > prefix_directory.size()) {
                return absl::InternalError("invalid prefix_id");
            }
            result.append(reinterpret_cast<const char*>(compressed_data.data() + offset),
                          suffix_len);
            return result;
        }
        offset += suffix_len;
    }

    return absl::InternalError("unreachable");
}

absl::StatusOr<std::vector<std::string>> MultiPrefixCompressor::decompress_all(
    std::span<const std::byte> compressed_data,
    const std::vector<std::string>& prefix_directory,
    size_t num_strings) {
    std::vector<std::string> results;
    results.reserve(num_strings);

    size_t offset = 0;
    for (size_t i = 0; i < num_strings; ++i) {
        if (offset >= compressed_data.size()) {
            return absl::InternalError("compressed data truncated");
        }
        uint32_t prefix_id = read_varint(compressed_data, offset);
        uint32_t suffix_len = read_varint(compressed_data, offset);

        if (offset + suffix_len > compressed_data.size()) {
            return absl::InternalError("compressed data truncated");
        }

        std::string result;
        if (prefix_id > 0 && prefix_id <= prefix_directory.size()) {
            result = prefix_directory[prefix_id - 1];
        } else if (prefix_id > prefix_directory.size()) {
            return absl::InternalError("invalid prefix_id");
        }
        result.append(reinterpret_cast<const char*>(compressed_data.data() + offset), suffix_len);
        results.push_back(std::move(result));
        offset += suffix_len;
    }

    return results;
}

} // namespace pl::sstv2::compress
