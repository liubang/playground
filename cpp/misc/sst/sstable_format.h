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

#pragma once

#include "cpp/misc/fs/fs.h"
#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

#include <cstdint>
#include <string>

namespace pl {

/**
 * @class BlockHandle
 * @brief 主要是用来构造索引的
 */
class BlockHandle {
public:
    enum { kMaxEncodedLength = 10 + 10 };

    BlockHandle() : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

    [[nodiscard]] uint64_t offset() const { return offset_; }

    [[nodiscard]] uint64_t size() const { return size_; }

    void setOffset(uint64_t offset) { offset_ = offset; }

    void setSize(uint64_t size) { size_ = size; }

    void encodeTo(std::string* dst) const;

    [[nodiscard]] Status decodeFrom(const Binary& input);

private:
    uint64_t offset_;
    uint64_t size_;
};

class Footer {
public:
    enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

    Footer() = default;

    void setMetaindexHandle(const BlockHandle& block_handle) { metaindex_handle_ = block_handle; }
    void setIndexHandle(const BlockHandle& block_handle) { index_handle_ = block_handle; }

    [[nodiscard]] const BlockHandle& metaindexHandle() const { return metaindex_handle_; }
    [[nodiscard]] const BlockHandle& indexHandle() const { return index_handle_; }

    void encodeTo(std::string* dst) const;
    [[nodiscard]] Status decodeFrom(const Binary& input);

private:
    BlockHandle metaindex_handle_;
    BlockHandle index_handle_;
};

static constexpr uint64_t kTableMagicNumber = 0x833859d02c1dbd75ull;

// compression type(1B) + crc(4B)
static constexpr uint32_t kBlockTrailerSize = 5;

struct BlockContents {
    Binary data;
    bool cachable;
    bool heap_allocated;
};

class BlockReader {
public:
    static Status readBlock(FsReader* reader, const BlockHandle& handle, BlockContents* result);
};

} // namespace pl
