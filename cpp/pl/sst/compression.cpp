// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/03/21 20:29

#include "cpp/pl/sst/compression.h"

#include <zstd.h>

#include "snappy.h"

namespace pl {

Result<Void> SnappyCompressionAdapter::compress(std::string_view input, std::string* output) {
    size_t max_len = snappy::MaxCompressedLength(input.size());
    output->resize(max_len);
    size_t output_len = 0;
    snappy::RawCompress(input.data(), input.size(), output->data(), &output_len);
    output->resize(output_len);
    RETURN_VOID;
}

Result<Void> SnappyCompressionAdapter::uncompress(std::string_view input, std::string* output) {
    size_t ulen;
    if (!snappy::GetUncompressedLength(input.data(), input.size(), &ulen)) {
        return makeError(StatusCode::kDataCorruption);
    }
    auto ubuf = std::make_unique<char[]>(ulen);
    if (!snappy::RawUncompress(input.data(), input.size(), ubuf.get())) {
        return makeError(StatusCode::kDataCorruption);
    }
    output->assign(ubuf.release(), ulen);

    RETURN_VOID;
}

Result<Void> ZstdCompressionAdapter::compress(std::string_view input, std::string* output) {
    size_t max_len = ZSTD_compressBound(input.size());
    output->resize(max_len);
    size_t compressed_size =
        ZSTD_compress(output->data(), max_len, input.data(), input.size(), 1 /* level */);
    if (ZSTD_isError(compressed_size) != 0u) {
        return makeError(StatusCode::kDataCorruption, "zstd compress error");
    }
    output->resize(compressed_size);
    RETURN_VOID;
}

Result<Void> ZstdCompressionAdapter::uncompress(std::string_view input, std::string* output) {
    size_t ulen = ZSTD_getFrameContentSize(input.data(), input.size());
    if (ulen == ZSTD_CONTENTSIZE_UNKNOWN || ulen == ZSTD_CONTENTSIZE_ERROR) {
        return makeError(StatusCode::kDataCorruption, "zstd: cannot determine uncompressed size");
    }
    output->resize(ulen);
    size_t decompressed_size = ZSTD_decompress(output->data(), ulen, input.data(), input.size());
    if (ZSTD_isError(decompressed_size) != 0u) {
        return makeError(StatusCode::kDataCorruption, "zstd decompress error");
    }
    output->resize(decompressed_size);
    RETURN_VOID;
}

Result<Void> IsalCompressionAdapter::compress(std::string_view /*input*/, std::string* /*output*/) {
    return makeError(StatusCode::kNotImplemented, "ISAL compression not implemented");
}

Result<Void> IsalCompressionAdapter::uncompress(std::string_view /*input*/,
                                                std::string* /*output*/) {
    return makeError(StatusCode::kNotImplemented, "ISAL decompression not implemented");
}

} // namespace pl
