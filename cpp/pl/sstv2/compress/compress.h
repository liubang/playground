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
// Created: 2026/06/05 22:09

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace pl::sstv2::compress {

enum class Codec : uint8_t {
    kNone = 0,
    kSnappy = 1,
    kZstd = 2,
};

struct Options {
    Codec codec = Codec::kNone;
    int zstd_level = 3;
};

struct Buffer {
    std::string bytes;
    Codec codec = Codec::kNone;
    uint64_t uncompressed_size = 0;

    [[nodiscard]] std::string_view view() const noexcept { return bytes; }
};

template <Codec C> struct CodecTraits;

template <> struct CodecTraits<Codec::kNone> {
    static constexpr Codec kCodec = Codec::kNone;
    static constexpr bool kUsesLevel = false;
    static constexpr std::string_view kName = "none";
};

template <> struct CodecTraits<Codec::kSnappy> {
    static constexpr Codec kCodec = Codec::kSnappy;
    static constexpr bool kUsesLevel = false;
    static constexpr std::string_view kName = "snappy";
};

template <> struct CodecTraits<Codec::kZstd> {
    static constexpr Codec kCodec = Codec::kZstd;
    static constexpr bool kUsesLevel = true;
    static constexpr std::string_view kName = "zstd";
};

namespace detail {

[[nodiscard]] absl::StatusOr<std::string> compress_none(std::string_view input, int level);
[[nodiscard]] absl::StatusOr<std::string> compress_snappy(std::string_view input, int level);
[[nodiscard]] absl::StatusOr<std::string> compress_zstd(std::string_view input, int level);

[[nodiscard]] absl::StatusOr<std::string> uncompress_none(std::string_view input,
                                                          uint64_t uncompressed_size);
[[nodiscard]] absl::StatusOr<std::string> uncompress_snappy(std::string_view input,
                                                            uint64_t uncompressed_size);
[[nodiscard]] absl::StatusOr<std::string> uncompress_zstd(std::string_view input,
                                                          uint64_t uncompressed_size);

} // namespace detail

template <Codec C> class CodecImpl {
public:
    using traits = CodecTraits<C>;

    [[nodiscard]] static absl::StatusOr<Buffer> compress(std::string_view input,
                                                         const Options& options = {}) {
        auto bytes = compress_bytes(input, options.zstd_level);
        if (!bytes.ok())
            return bytes.status();
        return Buffer{
            .bytes = std::move(*bytes),
            .codec = C,
            .uncompressed_size = input.size(),
        };
    }

    [[nodiscard]] static absl::StatusOr<std::string> uncompress(std::string_view input,
                                                                uint64_t uncompressed_size) {
        return uncompress_bytes(input, uncompressed_size);
    }

private:
    [[nodiscard]] static absl::StatusOr<std::string> compress_bytes(std::string_view input,
                                                                    int level) {
        if constexpr (C == Codec::kNone) {
            return detail::compress_none(input, level);
        } else if constexpr (C == Codec::kSnappy) {
            return detail::compress_snappy(input, level);
        } else if constexpr (C == Codec::kZstd) {
            return detail::compress_zstd(input, level);
        }
    }

    [[nodiscard]] static absl::StatusOr<std::string> uncompress_bytes(std::string_view input,
                                                                      uint64_t uncompressed_size) {
        if constexpr (C == Codec::kNone) {
            return detail::uncompress_none(input, uncompressed_size);
        } else if constexpr (C == Codec::kSnappy) {
            return detail::uncompress_snappy(input, uncompressed_size);
        } else if constexpr (C == Codec::kZstd) {
            return detail::uncompress_zstd(input, uncompressed_size);
        }
    }
};

template <Codec C>
[[nodiscard]] absl::StatusOr<Buffer> compress_as(std::string_view input,
                                                 const Options& options = {}) {
    return CodecImpl<C>::compress(input, options);
}

[[nodiscard]] absl::StatusOr<Buffer> compress_to_buffer(std::string_view input,
                                                        const Options& options);

[[nodiscard]] absl::StatusOr<std::string> compress(std::string_view input, const Options& options);

[[nodiscard]] absl::StatusOr<std::string> uncompress(std::string_view input,
                                                     Codec codec,
                                                     uint64_t uncompressed_size);

namespace block_flags {

static constexpr uint64_t kPatternStore = 1ULL << 0;
static constexpr uint64_t kRowKeyBitmap = 1ULL << 1;
static constexpr uint8_t kCompressShift = 2;
static constexpr uint64_t kCompressMask = 0xFFULL << kCompressShift;

} // namespace block_flags

[[nodiscard]] constexpr uint64_t encode_block_flag(Codec codec) {
    return block_flags::kPatternStore |
           (static_cast<uint64_t>(codec) << block_flags::kCompressShift);
}

[[nodiscard]] constexpr Codec decode_block_flag(uint64_t flags) {
    return static_cast<Codec>((flags & block_flags::kCompressMask) >> block_flags::kCompressShift);
}

} // namespace pl::sstv2::compress
