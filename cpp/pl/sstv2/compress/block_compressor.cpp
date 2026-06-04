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

#include "cpp/pl/sstv2/compress/block_compressor.h"

#include "absl/status/status.h"
#include "snappy.h"
#include "zstd.h"

namespace pl::sstv2::compress {

absl::StatusOr<std::string> BlockCompressor::compress(CompressionType type,
                                                      std::span<const std::byte> input) {
    switch (type) {
        case CompressionType::kNone: {
            return std::string(reinterpret_cast<const char*>(input.data()), input.size());
        }
        case CompressionType::kSnappy: {
            std::string output;
            snappy::Compress(reinterpret_cast<const char*>(input.data()), input.size(), &output);
            return output;
        }
        case CompressionType::kZstd: {
            size_t bound = ZSTD_compressBound(input.size());
            std::string output(bound, '\0');
            size_t compressed_size =
                ZSTD_compress(output.data(), bound, input.data(), input.size(), /*level=*/3);
            if (ZSTD_isError(compressed_size)) {
                return absl::InternalError(std::string("zstd compression failed: ") +
                                           ZSTD_getErrorName(compressed_size));
            }
            output.resize(compressed_size);
            return output;
        }
    }
    return absl::InternalError("unknown compression type");
}

absl::StatusOr<std::string> BlockCompressor::decompress(CompressionType type,
                                                        std::span<const std::byte> compressed,
                                                        size_t uncompressed_size) {
    switch (type) {
        case CompressionType::kNone: {
            return std::string(reinterpret_cast<const char*>(compressed.data()), compressed.size());
        }
        case CompressionType::kSnappy: {
            std::string output;
            if (!snappy::Uncompress(
                    reinterpret_cast<const char*>(compressed.data()), compressed.size(), &output)) {
                return absl::InternalError("snappy decompression failed");
            }
            return output;
        }
        case CompressionType::kZstd: {
            std::string output(uncompressed_size, '\0');
            size_t decompressed_size = ZSTD_decompress(
                output.data(), uncompressed_size, compressed.data(), compressed.size());
            if (ZSTD_isError(decompressed_size)) {
                return absl::InternalError(std::string("zstd decompression failed: ") +
                                           ZSTD_getErrorName(decompressed_size));
            }
            output.resize(decompressed_size);
            return output;
        }
    }
    return absl::InternalError("unknown compression type");
}

size_t BlockCompressor::max_compressed_size(CompressionType type, size_t input_size) {
    switch (type) {
        case CompressionType::kNone:
            return input_size;
        case CompressionType::kSnappy:
            return snappy::MaxCompressedLength(input_size);
        case CompressionType::kZstd:
            return ZSTD_compressBound(input_size);
    }
    return input_size;
}

} // namespace pl::sstv2::compress
