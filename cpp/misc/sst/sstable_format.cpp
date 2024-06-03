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

#include <snappy.h>

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

/*
 * footer format
 * +-----------------+-------------+--------------+-------------------+
 * | metaindex (16B) | index (16B) | padding (8B) | magic number (8B) |
 * +-----------------+-------------+--------------+-------------------+
 * | <------------------  40B ------------------> |
 *
 */
void Footer::encodeTo(std::string* dst) const {
    const std::size_t s = dst->size();
    metaindex_handle_.encodeTo(dst);
    index_handle_.encodeTo(dst);
    dst->resize(2 * BlockHandle::kMaxEncodedLength);
    encodeInt(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
    encodeInt(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
    assert(dst->size() == s + kEncodedLength);
}

Status Footer::decodeFrom(const Binary& input) {
    if (input.size() < kEncodedLength) {
        return Status::NewCorruption("invalid sstable format");
    }
    const char* magic_ptr = input.data() + kEncodedLength - 8;
    const auto magic_lo = decodeInt<uint32_t>(magic_ptr);
    const auto magic_hi = decodeInt<uint32_t>(magic_ptr + 4);
    const uint64_t magic =
        ((static_cast<uint64_t>(magic_hi) << 32 | (static_cast<uint64_t>(magic_lo))));
    if (magic != kTableMagicNumber) {
        return Status::NewCorruption("invalid magic number");
    }
    Status result = metaindex_handle_.decodeFrom(input);
    if (result.isOk()) {
        // TODO(liubang): 优化没必要的拷贝
        result = index_handle_.decodeFrom(Binary(input.data() + 16, 16));
    }
    return result;
}

Status BlockReader::readBlock(const FsReaderRef& reader,
                              const BlockHandle& handle,
                              BlockContents* result) {
    // read block trailer
    auto s = static_cast<std::size_t>(handle.size());
    char* buf = new char[s + kBlockTrailerSize];

    Binary content;
    auto status = reader->read(handle.offset(), s + kBlockTrailerSize, &content, buf);
    if (!status.isOk()) {
        delete[] buf;
        return status;
    }
    // invalid content
    if (content.size() != s + kBlockTrailerSize) {
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
        size_t len;
        if (!snappy::GetUncompressedLength(data, s, &len)) {
            delete[] buf;
            return Status::NewCorruption("invalid data");
        }
        char* new_buf = new char[len];
        if (!snappy::RawUncompress(data, s, new_buf)) {
            delete[] buf;
            delete[] new_buf;
            return Status::NewCorruption("invalid data");
        }
        delete[] buf;
        result->data = Binary(new_buf, len);
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
