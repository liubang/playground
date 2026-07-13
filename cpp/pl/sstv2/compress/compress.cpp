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

#include "cpp/pl/sstv2/compress/compress.h"

#include <limits>
#include <zstd.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "snappy.h"

namespace pl::sstv2::compress {

namespace detail {

constexpr uint64_t kMaxUncompressedSize = 1ULL << 30;

absl::Status validate_uncompressed_size(uint64_t uncompressed_size) {
    if (uncompressed_size > kMaxUncompressedSize ||
        uncompressed_size > std::numeric_limits<size_t>::max()) {
        return absl::ResourceExhaustedError("uncompressed size exceeds limit");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> compress_none(std::string_view input, int /*level*/) {
    return std::string(input);
}

absl::StatusOr<std::string> compress_snappy(std::string_view input, int /*level*/) {
    std::string out;
    snappy::Compress(input.data(), input.size(), &out);
    return out;
}

absl::StatusOr<std::string> compress_zstd(std::string_view input, int level) {
    const size_t bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) != 0) {
        return absl::InternalError("zstd compressBound failed");
    }

    std::string out(bound, '\0');
    const size_t n = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
    if (ZSTD_isError(n) != 0) {
        return absl::InternalError(absl::StrCat("zstd compress failed: ", ZSTD_getErrorName(n)));
    }
    out.resize(n);
    return out;
}

absl::StatusOr<std::string> uncompress_none(std::string_view input, uint64_t uncompressed_size) {
    if (input.size() != uncompressed_size) {
        return absl::InvalidArgumentError("uncompressed size mismatch");
    }
    return std::string(input);
}

absl::StatusOr<std::string> uncompress_snappy(std::string_view input, uint64_t uncompressed_size) {
    if (auto status = validate_uncompressed_size(uncompressed_size); !status.ok()) {
        return status;
    }
    size_t actual = 0;
    if (!snappy::GetUncompressedLength(input.data(), input.size(), &actual)) {
        return absl::InvalidArgumentError("invalid snappy stream");
    }
    if (actual != uncompressed_size) {
        return absl::InvalidArgumentError("snappy uncompressed size mismatch");
    }

    std::string out(actual, '\0');
    if (!snappy::RawUncompress(input.data(), input.size(), out.data())) {
        return absl::InvalidArgumentError("snappy uncompress failed");
    }
    return out;
}

absl::StatusOr<std::string> uncompress_zstd(std::string_view input, uint64_t uncompressed_size) {
    if (auto status = validate_uncompressed_size(uncompressed_size); !status.ok()) {
        return status;
    }
    std::string out(static_cast<size_t>(uncompressed_size), '\0');
    const size_t n = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
    if (ZSTD_isError(n) != 0) {
        return absl::InvalidArgumentError(
            absl::StrCat("zstd uncompress failed: ", ZSTD_getErrorName(n)));
    }
    if (n != uncompressed_size) {
        return absl::InvalidArgumentError("zstd uncompressed size mismatch");
    }
    return out;
}

} // namespace detail

absl::StatusOr<Buffer> compress_to_buffer(std::string_view input, const Options& options) {
    switch (options.codec) {
        case Codec::kNone:
            return CodecImpl<Codec::kNone>::compress(input, options);
        case Codec::kSnappy:
            return CodecImpl<Codec::kSnappy>::compress(input, options);
        case Codec::kZstd:
            return CodecImpl<Codec::kZstd>::compress(input, options);
    }
    return absl::InvalidArgumentError("unknown compression codec");
}

absl::StatusOr<std::string> compress(std::string_view input, const Options& options) {
    auto buffer = compress_to_buffer(input, options);
    if (!buffer.ok()) {
        return buffer.status();
    }
    return std::move(buffer->bytes);
}

absl::StatusOr<std::string> uncompress(std::string_view input,
                                       Codec codec,
                                       uint64_t uncompressed_size) {
    switch (codec) {
        case Codec::kNone:
            return CodecImpl<Codec::kNone>::uncompress(input, uncompressed_size);
        case Codec::kSnappy:
            return CodecImpl<Codec::kSnappy>::uncompress(input, uncompressed_size);
        case Codec::kZstd:
            return CodecImpl<Codec::kZstd>::uncompress(input, uncompressed_size);
    }
    return absl::InvalidArgumentError("unknown compression codec");
}

} // namespace pl::sstv2::compress
