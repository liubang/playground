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

#include "cpp/misc/sst/sstable_format.h"
#include "cpp/misc/sst/encoding.h"
#include "cpp/misc/sst/options.h"
#include "cpp/tools/crc.h"

#include "snappy.h"
#include <zstd.h>

namespace pl {

void BlockHandle::encodeTo(std::string* dst) const {
    assert(offset_ != ~static_cast<uint64_t>(0));
    assert(size_ != ~static_cast<uint64_t>(0));
    encodeInt(dst, offset_);
    encodeInt(dst, size_);
}

Status BlockHandle::decodeFrom(const Binary& input) {
    if (input.size() < 16) {
        return Status::NewCorruption("bad block handle");
    }
    offset_ = decodeInt<uint64_t>(input.data());
    size_ = decodeInt<uint64_t>(input.data() + 8);
    return Status::NewOk();
}

void FileMeta::encodeTo(std::string* dst) const {
    assert(dst != nullptr);
    encodeInt(dst, static_cast<uint8_t>(sst_type_));        // 1B
    encodeInt(dst, static_cast<uint8_t>(sst_version_));     // 1B
    encodeInt(dst, key_number_);                            // 8B
    encodeInt(dst, bits_per_key_);                          // 4B
    encodeInt(dst, static_cast<uint32_t>(min_key_.size())); // 4B
    dst->append(min_key_);
    encodeInt(dst, static_cast<uint32_t>(max_key_.size())); // 4B
    dst->append(max_key_);
}

Status FileMeta::decodeFrom(const Binary& input) {
    if (input.size() < FILE_META_MIN_LEN) {
        return Status::NewCorruption("invalid sstable format");
    }
    const char* data = input.data();
    const auto s = input.size();
    uint8_t type = decodeInt<uint8_t>(data);
    if (type == 0 || type > static_cast<uint8_t>(SSTType::MAJOR)) {
        return Status::NewCorruption("invalid sstable type");
    }
    sst_type_ = static_cast<SSTType>(type);
    uint8_t version = decodeInt<uint8_t>(data + 1);
    if (version == 0 || version > static_cast<uint8_t>(SSTVersion::V1)) {
        return Status::NewCorruption("invalid sstable version");
    }
    sst_version_ = static_cast<SSTVersion>(version);
    key_number_ = decodeInt<uint64_t>(data + 2);
    bits_per_key_ = decodeInt<uint32_t>(data + 10);
    uint32_t min_key_size = decodeInt<uint32_t>(data + 14);

    if (18 + min_key_size + 4 > s) {
        return Status::NewCorruption("invalid sstable format");
    }

    if (min_key_size > 0) {
        min_key_.assign(data + 18, min_key_size);
    }
    uint32_t max_key_size = decodeInt<uint32_t>(data + 18 + min_key_size);

    if (18 + min_key_size + 4 + max_key_size != s) {
        return Status::NewCorruption("invalid sstable format");
    }

    if (max_key_size > 0) {
        max_key_.assign(data + 18 + min_key_size + 4, max_key_size);
    }

    return Status::NewOk();
}

/**
 * footer format
 * +-----------------+-------------+----------------+---------+-------------------+
 * | metaindex (16B) | index (16B) | filemeta (16B) | padding | magic number (8B) |
 * +-----------------+-------------+----------------+---------+-------------------+
 * | <------------------------- 56B ------------------------> |
 * | <------------------------------------ 64B ---------------------------------->|
 *
 */
void Footer::encodeTo(std::string* dst) const {
    assert(dst != nullptr);
    const std::size_t s = dst->size();
    filter_handle_.encodeTo(dst);
    index_handle_.encodeTo(dst);
    file_meta_handle_.encodeTo(dst);
    dst->resize(FOOTER_LEN - 8);
    encodeInt(dst, static_cast<uint32_t>(SST_MAGIC_NUMBER & 0xffffffffu));
    encodeInt(dst, static_cast<uint32_t>(SST_MAGIC_NUMBER >> 32));
    assert(dst->size() == s + FOOTER_LEN);
}

Status Footer::decodeFrom(const Binary& input) {
    if (input.size() < FOOTER_LEN) {
        return Status::NewCorruption("invalid sstable format");
    }
    const char* magic_ptr = input.data() + FOOTER_LEN - 8;
    const auto magic_lo = decodeInt<uint32_t>(magic_ptr);
    const auto magic_hi = decodeInt<uint32_t>(magic_ptr + 4);
    const uint64_t magic =
        ((static_cast<uint64_t>(magic_hi) << 32 | (static_cast<uint64_t>(magic_lo))));
    if (magic != SST_MAGIC_NUMBER) {
        return Status::NewCorruption("invalid magic number");
    }

    // decode metaindex offset and size
    Status result = filter_handle_.decodeFrom(input);
    if (!result.isOk()) {
        return result;
    }

    // decode index offset and size
    result = index_handle_.decodeFrom(Binary(input.data() + 16, 16));
    if (!result.isOk()) {
        return result;
    }

    // decode file meta offset and size
    result = file_meta_handle_.decodeFrom(Binary(input.data() + 32, 16));
    return result;
}

Status BlockReader::readBlock(const FsReaderRef& reader,
                              const BlockHandle& handle,
                              BlockContents* result) {
    // read block trailer
    auto s = static_cast<std::size_t>(handle.size());
    char* buf = new char[s + BLOCK_TRAILER_LEN];

    Binary content;
    auto status = reader->read(handle.offset(), s + BLOCK_TRAILER_LEN, &content, buf);
    if (!status.isOk()) {
        delete[] buf;
        return status;
    }
    // invalid content
    if (content.size() != s + BLOCK_TRAILER_LEN) {
        delete[] buf;
        return Status::NewCorruption("invalid block");
    }

    // crc check
    const char* data = content.data();
    auto crc = decodeInt<uint32_t>(data + s + 1);
    auto actual_crc = crc32(data, s);
    if (crc != actual_crc) {
        delete[] buf;
        return Status::NewCorruption("crc error");
    }
    switch (static_cast<CompressionType>(data[s])) {
    case CompressionType::kSnappyCompression:
    {
        size_t ulen;
        if (!snappy::GetUncompressedLength(data, s, &ulen)) {
            delete[] buf;
            return Status::NewCorruption("invalid data");
        }
        char* ubuf = new char[ulen];
        if (!snappy::RawUncompress(data, s, ubuf)) {
            delete[] buf;
            delete[] ubuf;
            return Status::NewCorruption("invalid data");
        }
        delete[] buf;
        result->data = Binary(ubuf, ulen);
        result->heap_allocated = true;
        result->cachable = true;
        break;
    }
    case CompressionType::kZstdCompression:
    {
        size_t ulen = ZSTD_getFrameContentSize(data, s);
        if (ulen == 0) {
            delete[] buf;
            return Status::NewCorruption("invalid data");
        }
        char* ubuf = new char[ulen];
        ZSTD_DCtx* ctx = ZSTD_createDCtx();
        size_t outlen = ZSTD_decompressDCtx(ctx, ubuf, ulen, data, s);
        ZSTD_freeDCtx(ctx);
        if (ZSTD_isError(outlen) != 0u) {
            delete[] buf;
            delete[] ubuf;
            return Status::NewCorruption("invalid data");
        }
        delete[] buf;
        result->data = Binary(ubuf, ulen);
        result->heap_allocated = true;
        result->cachable = true;
        break;
    }
    default:
    {
        result->data = Binary(buf, s);
        result->heap_allocated = true;
        result->cachable = true;
        break;
    }
    }

    return Status::NewOk();
}

} // namespace pl
