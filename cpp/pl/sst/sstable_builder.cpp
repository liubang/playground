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

#include "cpp/pl/sst/sstable_builder.h"
#include "cpp/pl/fs/posix_fs.h"
#include "cpp/pl/sst/encoding.h"

#include "snappy.h"
#include <cassert>
#include <isa-l.h>
#include <utility>
#include <zstd.h>

namespace pl {

SSTableBuilder::SSTableBuilder(BuildOptionsRef options) : options_(std::move(options)) {}

Result<Void> SSTableBuilder::open() {
    // TODO(liubang): check option
    data_block_ = std::make_unique<BlockBuilder>(options_);
    index_block_ = std::make_unique<BlockBuilder>(options_);
    filter_block_ = options_->filter_policy == nullptr
                        ? nullptr
                        : std::make_unique<FilterBlockBuilder>(options_->filter_policy);

    sst_file_ = (options_->data_dir / SSTType2String(options_->sst_type) /
                 (std::to_string(options_->sst_id) + ".sst"))
                    .string();

    writer_ = std::make_unique<PosixFileSystem>();
    auto result = writer_->open(sst_file_, O_TRUNC | O_WRONLY | O_CREAT, &fd_);
    RETURN_AND_LOG_ON_ERROR(result);
    RETURN_VOID;
}

Result<Void> SSTableBuilder::add(const Cell& cell) {
    assert(!closed_);
    if (cell_nums_ == 0) {
        first_key_.assign(cell.rowkey());
    }

    int comp = 0;
    if (cell_nums_ > 0) {
        comp = options_->comparator->compare(cell.rowkey(), last_key_);
    } else {
        comp = 1;
    }
    // 数据递增, = 0 表示同一行的不同列，或者同一列的不同版本
    assert(comp >= 0);

    // 同一行的所有列都在同一个data block中，因为索引是基于rowkey构建的
    // 如果此时换行且达到block_size限制后，就先flush
    if (comp > 0) {
        const size_t s = data_block_->sizeEstimate();
        if (s >= options_->block_size) {
            flush();
        }
    }

    if (pending_index_entry_) {
        assert(data_block_->empty());
        options_->comparator->findShortestSeparator(&last_key_, cell.rowkey());
        std::string handle_encoding;
        pending_handler_.encodeTo(&handle_encoding);
        // 记录上一个block的最后一个key和上一个block的结束位置
        index_block_->add(Cell(CellType::CT_NONE, last_key_, "", "", handle_encoding, 0));
        pending_index_entry_ = false;
    }

    if (comp > 0) {
        if (filter_block_ != nullptr) {
            filter_block_->addKey(cell.rowkey());
        }
        last_key_.assign(cell.rowkey());
        row_num_++;
    }

    min_timestamp_ = std::min(min_timestamp_, cell.timestamp());
    max_timestamp_ = std::max(max_timestamp_, cell.timestamp());
    assert(min_timestamp_ <= max_timestamp_);

    data_block_->add(cell);
    cell_nums_++;

    RETURN_VOID;
}

Result<Void> SSTableBuilder::flush() {
    assert(!closed_);
    if (data_block_->empty()) {
        RETURN_VOID;
    }
    // 写入block
    auto result = writeBlock(data_block_.get(), &pending_handler_);
    RETURN_AND_LOG_ON_ERROR(result);
    // 复位标记，下次开始一个新的block
    pending_index_entry_ = true;
    // status_ = writer_->fsync();
    if (filter_block_ != nullptr) {
        filter_block_->startBlock(offset_);
    }
    RETURN_VOID;
}

Result<Void> SSTableBuilder::writeBlock(BlockBuilder* block, BlockHandle* handle) {
    auto raw = block->finish();
    std::string compressed;
    switch (options_->compression_type) {
    case CompressionType::SNAPPY:
    {
        auto outlen = snappy::Compress(raw.data(), raw.size(), &compressed);
        compressed.resize(outlen);
        raw = compressed;
        break;
    }
    case CompressionType::ZSTD:
    {
        size_t outlen = ZSTD_compressBound(raw.size());
        if (ZSTD_isError(outlen) != 0u) {
            // TODO(liubang): error handler
            assert(false);
        }
        compressed.resize(outlen);
        ZSTD_CCtx* ctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, options_->zstd_compress_level);
        outlen = ZSTD_compress2(ctx, &(compressed)[0], compressed.size(), raw.data(), raw.size());
        ZSTD_freeCCtx(ctx);
        if (ZSTD_isError(outlen) != 0u) {
            // TODO(liubang): error handler
            assert(false);
        }
        compressed.resize(outlen);
        raw = compressed;
        break;
    }
    case CompressionType::ISAL:
    {
        // TODO(liubang):
    }
    default:
        break;
    }
    auto result = writeBlockRaw(raw, options_->compression_type, handle);
    block->reset();
    return result;
}

/**
 *
 * the block format
 *
 *
 *                                                     +---------------------+
 *                                                     |  shared size(4B)    |
 *                                                     +---------------------+
 *                                                     | non shared size(4B) |
 *                                  +----------------->+---------------------+
 *                                  |                  |   rowkey size(4B)   |
 *                                  |                  +---------------------+
 *                                  |                  |   value size(4B)    |
 *                                  |                  +---------------------+
 *                                  |                  |   non shared key    |
 *                                  |                  +---------------------+
 *                                  |                  |        value        |
 *                                  |                  +---------------------+
 * +------------------+             |
 * |  <key1, value2>  +-------------+
 * +------------------+
 * |  <key2, value2>  |
 * +------------------+
 * |    ......        |
 * +------------------+                                +-------------------+
 * |  restart idxes   +------------------------------->|   restart[0](4B)  |
 * +------------------+                                +-------------------+
 * |  block trailer   |                                |   restart[1](4B)  |
 * +-------+----------+                                +-------------------+
 *         |                                           |      ......       |
 *         |                                           +-------------------+
 *         |                                           |   restart[n](4B)  |
 *         |                                           +-------------------+
 *         |                                           |  restart count(4B)|
 *         |                                           +-------------------+
 *         |
 *         |
 *         |
 *         |                                         +-----------------------+
 *         +---------------------------------------->| compresstion type(1B) |
 *                                                   +-----------------------+
 *                                                   |      crc(4B)          |
 *                                                   +-----------------------+
 *
 *
 */
Result<Void> SSTableBuilder::writeBlockRaw(std::string_view content,
                                           CompressionType type,
                                           BlockHandle* handle) {
    handle->setOffset(offset_);
    handle->setSize(content.size());
    auto result = writer_->append(fd_, 0, content);
    RETURN_AND_LOG_ON_ERROR(result);

    // crc
    char trailer[BLOCK_TRAILER_LEN];
    trailer[0] = static_cast<const char>(type);
    uint32_t crc = ::crc32_iscsi((unsigned char*)content.data(), content.size(), 0);
    std::string encode_crc;
    encodeInt<uint32_t>(&encode_crc, crc);
    memcpy(trailer + 1, encode_crc.data(), encode_crc.size());
    result = writer_->append(fd_, 0, std::string_view(trailer, BLOCK_TRAILER_LEN));
    RETURN_AND_LOG_ON_ERROR(result);

    // 更新下一个block的offset
    offset_ += content.size() + BLOCK_TRAILER_LEN;
    RETURN_VOID;
}

/**
 * the sst file format:
 *
 *   +--------------------+
 *   |     data block     |
 *   +--------------------+
 *   |    filter block    |
 *   +--------------------+
 *   |     index block    |
 *   +--------------------+
 *   |   filemeta block   |
 *   +--------------------+
 *   |       footer       |
 *   +--------------------+
 */
Result<Void> SSTableBuilder::finish() {
    assert(!closed_);
    flush();
    closed_ = true;
    BlockHandle filter_block_handle;
    BlockHandle index_block_handle;
    BlockHandle file_meta_handle;

    // 写filter block
    if (filter_block_ != nullptr) {
        auto result =
            writeBlockRaw(filter_block_->finish(), CompressionType::NONE, &filter_block_handle);
        RETURN_AND_LOG_ON_ERROR(result);
    }

    // 写入index block
    // 处理边界情况
    if (pending_index_entry_) {
        options_->comparator->findShortSucessor(&last_key_);
        std::string handle_encoding;
        pending_handler_.encodeTo(&handle_encoding);
        // 记录上一个block的最后一个key和上一个block的结束位置
        index_block_->add(Cell(CellType::CT_NONE, last_key_, "", "", handle_encoding, 0));
        pending_index_entry_ = false;
    }
    auto result = writeBlock(index_block_.get(), &index_block_handle);
    RETURN_AND_LOG_ON_ERROR(result);

    // 写入file meta，使用block格式有crc校验
    FileMeta file_meta;
    file_meta.setSSTType(options_->sst_type);
    file_meta.setSSTVersion(options_->sst_version);
    file_meta.setFilterPolicyType(options_->filter_type);
    file_meta.setBitsPerKey(options_->bits_per_key);
    file_meta.setMinKey(first_key_);
    file_meta.setMaxKey(last_key_);
    file_meta.setCellNum(cell_nums_);
    file_meta.setRowNum(row_num_);
    file_meta.setMinTimestamp(min_timestamp_);
    file_meta.setMaxTimestamp(max_timestamp_);
    file_meta.setPatchId(options_->patch_id);
    file_meta.setSSTId(options_->sst_id);
    std::string file_meta_content;
    file_meta.encodeTo(&file_meta_content);
    result = writeBlockRaw(file_meta_content, CompressionType::NONE, &file_meta_handle);
    RETURN_AND_LOG_ON_ERROR(result);

    // 写入footer
    Footer footer;
    footer.setFileMetaHandle(file_meta_handle);
    footer.setFilterHandle(filter_block_handle);
    footer.setIndexHandle(index_block_handle);
    std::string footer_content;
    footer.encodeTo(&footer_content);
    result = writer_->append(fd_, 0, footer_content);
    RETURN_AND_LOG_ON_ERROR(result);
    offset_ += footer_content.size();
    result = writer_->fsync(fd_, 0);
    RETURN_AND_LOG_ON_ERROR(result);
    RETURN_VOID;
}

} // namespace pl
