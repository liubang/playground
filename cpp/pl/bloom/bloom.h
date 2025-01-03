// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/fastrange/fastrange.h"
#include "cpp/pl/lang/common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pl {

class BloomMath {
public:
    // Standard bloom filter false positive rate
    static double standard_fp_rate(double bits_per_key, int num_probes) {
        // Standard very-good-estimate formula. See
        // https://en.wikipedia.org/wiki/Bloom_filter#Probability_of_false_positives
        return std::pow(1.0 - std::exp(-num_probes / bits_per_key), num_probes);
    }

    static double blocked_bloom_fp_rate(double bits_per_key, int num_probes, int cache_line_bits) {
        if (bits_per_key <= 0.0) {
            return 1.0;
        }
        double keys_per_cache_line = cache_line_bits / bits_per_key;
        double keys_stddev = std::sqrt(keys_per_cache_line);
        double crowded_fp =
            standard_fp_rate(cache_line_bits / (keys_per_cache_line + keys_stddev), num_probes);
        double uncrowded_fp =
            standard_fp_rate(cache_line_bits / (keys_per_cache_line - keys_stddev), num_probes);
        return (crowded_fp + uncrowded_fp) / 2;
    }
};

// It consists of a sequence of block comparatively small standard Bloom filters, each of which fits
// into once cache-line. Nowadays, a common cache line size is 64 bytes, thus 512 bits. For best
// performance, those small filters are stored cache-line-aligned. For each potential element, the
// first hash value selects the Bloom filter block to be used. Additional hash values are then used
// to set or test bits as usual, but only inside this one block. A blocked Bloom filter therefore
// only needs one cache miss for every operation.
class BlockedBloomFilter {
public:
    static int choose_num_probes(int millibits_per_key) {
        // Since this implementation can (with AVX2) make up to 8 probes
        // for the same cost, we pick the most accurate num_probes, based
        // on actual tests of the implementation. Note that for higher
        // bits/key, the best choice for cache-local Bloom can be notably
        // smaller than standard bloom, e.g. 9 instead of 11 @ 16 b/k.
        if (millibits_per_key <= 2080) {
            return 1;
        }
        if (millibits_per_key <= 3580) {
            return 2;
        }
        if (millibits_per_key <= 5100) {
            return 3;
        }
        if (millibits_per_key <= 6640) {
            return 4;
        }
        if (millibits_per_key <= 8300) {
            return 5;
        }
        if (millibits_per_key <= 10070) {
            return 6;
        }
        if (millibits_per_key <= 11720) {
            return 7;
        }
        if (millibits_per_key <= 14001) {
            // Would be something like <= 13800 but sacrificing *slightly* for
            // more settings using <= 8 probes.
            return 8;
        }
        if (millibits_per_key <= 16050) {
            return 9;
        }
        if (millibits_per_key <= 18300) {
            return 10;
        }
        if (millibits_per_key <= 22001) {
            return 11;
        }
        if (millibits_per_key <= 25501) {
            return 12;
        }
        if (millibits_per_key > 50000) {
            // Top out at 24 probes (three sets of 8)
            return 24;
        }
        // Roughly optimal choices for remaining range
        // e.g.
        // 28000 -> 12, 28001 -> 13
        // 50000 -> 23, 50001 -> 24
        return (millibits_per_key - 1) / 2000 - 1;
    }

    static void add_hash(uint32_t h1, uint32_t h2, uint32_t bytes_len, int num_probes, char* data) {
        // Use h1 to choese which cache-line-aligned block to be used.
        uint32_t bytes_to_cache_line = fastrange32(h1, bytes_len >> 6) << 6;
        add_hash_prepared(h2, num_probes, data + bytes_to_cache_line);
    }

    static void add_hash_prepared(uint32_t h2, int num_probes, char* data_at_cache_line) {
        uint32_t h = h2;
        for (int i = 0; i < num_probes; ++i, h *= uint32_t{0x9e3779b9}) {
            // 9-bit address within 512 bit cache line
            int bitpos = h >> (32 - 9);
            data_at_cache_line[bitpos >> 3] |= (uint8_t{1} << (bitpos & 7));
        }
    }

    static void prepare_hash(uint32_t h1,
                             uint32_t bytes_len,
                             const char* data,
                             uint32_t* byte_offset) {
        uint32_t bytes_to_cache_line = fastrange32(h1, bytes_len >> 6) << 6;
        PL_PREFETCH(data + bytes_to_cache_line, 0 /* rw */, 1 /* locality */);
        PL_PREFETCH(data + bytes_to_cache_line + 63, 0 /* rw */, 1 /* locality */);
        *byte_offset = bytes_to_cache_line;
    }

    static bool hash_may_match(
        uint32_t h1, uint32_t h2, uint32_t bytes_len, int num_probes, const char* data) {
        // Use h1 to choese which cache-line-aligned block to be used.
        uint32_t bytes_to_cache_line = fastrange32(h1, bytes_len >> 6) << 6;
        return hash_may_match_prepared(h2, num_probes, data + bytes_to_cache_line);
    }

    static bool hash_may_match_prepared(uint32_t h2,
                                        int num_probes,
                                        const char* data_at_cache_line) {
        uint32_t h = h2;
        // TODO(liubang): use SIMD
        for (int i = 0; i < num_probes; ++i, h *= uint32_t{0x9e3779b9}) {
            // 9-bit address within 512 bit cache line
            int bitpos = h >> (32 - 9);
            if ((data_at_cache_line[bitpos >> 3] & (uint8_t{1} << (bitpos & 7))) == 0) {
                return false;
            }
        }

        return true;
    }
};

class StandardBloomFilter {
public:
    static int choose_num_probes(int bits_per_key) {
        // 0.69 =~ ln(2)
        return std::min(30, std::max(1, static_cast<int>(bits_per_key * 0.69)));
    }

    static void add_hash(uint32_t h, uint32_t total_bits, int num_probes, /* output */ char* data) {
        const uint32_t delta = (h >> 17) | (h << 15); // Rotate right 17 bits
        for (int i = 0; i < num_probes; ++i) {
            const uint32_t bitpos = h % total_bits;
            data[bitpos >> 3] |= (uint8_t{1} << (bitpos & 7));
            h += delta;
        }
    }

    static bool hash_may_match(uint32_t h, uint32_t total_bits, int num_probes, const char* data) {
        const uint32_t delta = (h >> 17) | (h << 15); // Rotate right 17 bits
        for (int i = 0; i < num_probes; ++i) {
            const uint32_t bitpos = h % total_bits;
            if ((data[bitpos >> 3] & (uint8_t{1} << (bitpos & 7))) == 0) {
                return false;
            }
            h += delta;
        }
        return true;
    }
};

} // namespace pl
