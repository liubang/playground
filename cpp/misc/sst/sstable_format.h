//=====================================================================
//
// sstable_format.h -
//
// Created by liubang on 2023/05/31 20:56
// Last Modified: 2023/05/31 20:56
//
//=====================================================================
#pragma once

#include "cpp/misc/fs/fs.h"
#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

#include <cstddef>
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

    void encodeTo(std::string *dst) const;
    [[nodiscard]] Status decodeFrom(const Binary &input);

private:
    uint64_t offset_;
    uint64_t size_;
};

class Footer {
public:
    enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

    Footer() = default;

    void setMetaindexHandle(const BlockHandle &block_handle) { metaindex_handle_ = block_handle; }
    void setIndexHandle(const BlockHandle &block_handle) { index_handle_ = block_handle; }

    [[nodiscard]] const BlockHandle &metaindexHandle() const { return metaindex_handle_; }
    [[nodiscard]] const BlockHandle &indexHandle() const { return index_handle_; }

    void encodeTo(std::string *dst) const;
    [[nodiscard]] Status decodeFrom(const Binary &input);

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
    static Status readBlock(FsReader *reader, const BlockHandle &handle, BlockContents *result);
};

} // namespace pl
