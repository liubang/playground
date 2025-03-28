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

#include "cpp/pl/sst/sstable_format.h"
#include "cpp/pl/sst/encoding.h"

#include "snappy.h"
#include <cassert>
#include <isa-l/crc.h>
#include <zstd.h>

namespace pl {

void BlockHandle::encodeTo(std::string* dst) const {
    assert(offset_ != ~static_cast<uint64_t>(0));
    assert(size_ != ~static_cast<uint64_t>(0));
    encodeInt(dst, offset_);
    encodeInt(dst, size_);
}

Status BlockHandle::decodeFrom(std::string_view input) {
    if (input.size() < 16) {
        return Status::NewCorruption("bad block handle");
    }
    offset_ = decodeInt<uint64_t>(input.data());
    size_ = decodeInt<uint64_t>(input.data() + 8);
    return Status::NewOk();
}

void FileMeta::encodeTo(std::string* dst) const {
    assert(dst != nullptr);
    encodeInt(dst, FILE_META_MAGIC_NUMBER);                 // 4B
    encodeInt(dst, static_cast<uint8_t>(sst_type_));        // 1B
    encodeInt(dst, static_cast<uint8_t>(sst_version_));     // 1B
    encodeInt(dst, static_cast<uint64_t>(patch_id_));       // 8B
    encodeInt(dst, static_cast<uint64_t>(sst_id_));         // 8B
    encodeInt(dst, static_cast<uint8_t>(filter_type_));     // 1B
    encodeInt(dst, bits_per_key_);                          // 4B
    encodeInt(dst, cell_number_);                           // 8B
    encodeInt(dst, row_number_);                            // 8B
    encodeInt(dst, min_timestamp_);                         // 8B
    encodeInt(dst, max_timestamp_);                         // 8B
    encodeInt(dst, static_cast<uint32_t>(min_key_.size())); // 4B
    dst->append(min_key_);
    encodeInt(dst, static_cast<uint32_t>(max_key_.size())); // 4B
    dst->append(max_key_);
}

Status FileMeta::decodeFrom(std::string_view input) {
    if (input.size() < FILE_META_MIN_LEN) {
        return Status::NewCorruption("file meta is too short");
    }
    const char* data = input.data();
    const auto s = input.size();
    uint32_t cursor = 0;

    // magic number
    auto magic = decodeInt<uint32_t>(data + cursor);
    cursor += 4;
    if (magic != FILE_META_MAGIC_NUMBER) {
        return Status::NewCorruption("invalid file meta, the magic number is mismatch");
    }

    // sst type
    auto type = decodeInt<uint8_t>(data + cursor);
    cursor++;
    if (type == 0 || type > static_cast<uint8_t>(SSTType::MAJOR)) {
        return Status::NewCorruption("invalid sstable type");
    }
    sst_type_ = static_cast<SSTType>(type);

    // sst version
    auto version = decodeInt<uint8_t>(data + cursor);
    cursor++;
    if (version == 0 || version > static_cast<uint8_t>(SSTVersion::V1)) {
        return Status::NewCorruption("invalid sstable version");
    }
    sst_version_ = static_cast<SSTVersion>(version);

    // patch_id
    patch_id_ = decodeInt<uint64_t>(data + cursor);
    cursor += 8;

    // sst id
    sst_id_ = decodeInt<uint64_t>(data + cursor);
    cursor += 8;

    // filter type
    auto filter_type = decodeInt<uint8_t>(data + cursor);
    cursor++;
    if (filter_type > static_cast<uint8_t>(FilterPolicyType::BLOOM_FILTER)) {
        return Status::NewCorruption("invalid filter policy type");
    }
    filter_type_ = static_cast<FilterPolicyType>(filter_type);

    // bits_per_key
    bits_per_key_ = decodeInt<uint32_t>(data + cursor);
    cursor += 4;

    // cell number
    cell_number_ = decodeInt<uint64_t>(data + cursor);
    cursor += 8;

    // row number
    row_number_ = decodeInt<uint64_t>(data + cursor);
    cursor += 8;

    // min timestamp
    min_timestamp_ = decodeInt<uint64_t>(data + cursor);
    cursor += 8;

    // max timestamp
    max_timestamp_ = decodeInt<uint64_t>(data + cursor);
    cursor += 8;

    // min key
    auto min_key_size = decodeInt<uint32_t>(data + cursor);
    cursor += 4;
    if (cursor + min_key_size + 4 > s) {
        return Status::NewCorruption("parse file meta error");
    }

    if (min_key_size > 0) {
        min_key_.assign(data + cursor, min_key_size);
        cursor += min_key_size;
    }

    // max key
    auto max_key_size = decodeInt<uint32_t>(data + cursor);
    cursor += 4;

    if (cursor + max_key_size != s) {
        return Status::NewCorruption("parse file meta error");
    }

    if (max_key_size > 0) {
        max_key_.assign(data + cursor, max_key_size);
    }

    return Status::NewOk();
}

/**
 * footer format
 * +-----------------------+----------------------+-------------------------+--------------------+
 * |  filter handle (16B)  |  index handle (16B)  |  filemata handle (16B)  |  magic number(4B)  |
 * +-----------------------+----------------------+-------------------------+--------------------+
 * | <-------------------------------- 56B -------------------------------> |
 * |
 * | <------------------------------------------- 60B ------------------------------------------>|
 *
 */
void Footer::encodeTo(std::string* dst) const {
    assert(dst != nullptr);
    const std::size_t s = dst->size();
    filter_handle_.encodeTo(dst);
    index_handle_.encodeTo(dst);
    file_meta_handle_.encodeTo(dst);
    dst->resize(FOOTER_LEN - sizeof(SST_MAGIC_NUMBER));
    encodeInt(dst, static_cast<uint32_t>(SST_MAGIC_NUMBER));
    assert(dst->size() == s + FOOTER_LEN);
}

Status Footer::decodeFrom(std::string_view input) {
    if (input.size() < FOOTER_LEN) {
        return Status::NewCorruption("invalid sstable format");
    }
    const char* magic_ptr = input.data() + FOOTER_LEN - sizeof(SST_MAGIC_NUMBER);
    const auto magic = decodeInt<uint32_t>(magic_ptr);

    if (magic != SST_MAGIC_NUMBER) {
        return Status::NewCorruption("invalid magic number");
    }

    // decode filter offset and size
    Status result = filter_handle_.decodeFrom(input);
    if (!result.isOk()) {
        return result;
    }

    // decode index offset and size
    result = index_handle_.decodeFrom(std::string_view(input.data() + 16, 16));
    if (!result.isOk()) {
        return result;
    }

    // decode file meta offset and size
    result = file_meta_handle_.decodeFrom(std::string_view(input.data() + 32, 16));
    return result;
}

Status BlockReader::readBlock(const FileSystemRef& reader,
                              const FileDescriptorRef& fd,
                              const BlockHandle& handle,
                              BlockContents* result) {
    // read block trailer
    auto s = static_cast<std::size_t>(handle.size());
    auto buf = std::make_unique<char[]>(s + BLOCK_TRAILER_LEN);

    std::string_view content;
    auto status = reader->pread(fd, handle.offset(), s + BLOCK_TRAILER_LEN, buf.get(), &content);
    if (!status.isOk()) {
        return status;
    }
    // invalid content
    if (content.size() != s + BLOCK_TRAILER_LEN) {
        return Status::NewCorruption("invalid block");
    }

    // crc check
    const char* data = content.data();
    auto crc = decodeInt<uint32_t>(data + s + 1);
    auto actual_crc = ::crc32_iscsi((unsigned char*)data, s, 0);
    if (crc != actual_crc) {
        return Status::NewCorruption("crc error");
    }
    switch (static_cast<CompressionType>(data[s])) {
    case CompressionType::SNAPPY:
    {
        size_t ulen;
        if (!snappy::GetUncompressedLength(data, s, &ulen)) {
            return Status::NewCorruption("invalid data");
        }
        auto ubuf = std::make_unique<char[]>(ulen);
        if (!snappy::RawUncompress(data, s, ubuf.get())) {
            return Status::NewCorruption("invalid data");
        }
        result->data = std::string_view(ubuf.release(), ulen);
        result->heap_allocated = true;
        result->cachable = true;
        break;
    }
    case CompressionType::ZSTD:
    {
        size_t ulen = ZSTD_getFrameContentSize(data, s);
        if (ulen == 0) {
            return Status::NewCorruption("invalid data");
        }
        auto ubuf = std::make_unique<char[]>(ulen);
        ZSTD_DCtx* ctx = ZSTD_createDCtx();
        size_t outlen = ZSTD_decompressDCtx(ctx, ubuf.get(), ulen, data, s);
        ZSTD_freeDCtx(ctx);
        if (ZSTD_isError(outlen) != 0u) {
            return Status::NewCorruption("invalid data");
        }
        result->data = std::string_view(ubuf.release(), ulen);
        result->heap_allocated = true;
        result->cachable = true;
        break;
    }
    case CompressionType::ISAL:
    {
        // TODO(liubang):
    }
    default:
    {
        result->data = std::string_view(buf.release(), s);
        result->heap_allocated = true;
        result->cachable = true;
        break;
    }
    }

    return Status::NewOk();
}

} // namespace pl
