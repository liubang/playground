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

#include "cpp/misc/sst/sstable_builder.h"
#include "cpp/misc/sst/encoding.h"
#include "cpp/tools/crc.h"

#include "snappy.h"
#include <zstd.h>

namespace pl {

SSTableBuilder::SSTableBuilder(const BuildOptionsRef& options, const FsWriterRef& writer)
    : options_(options),
      writer_(writer),
      data_block_(options),
      index_block_(options),
      filter_block_(options->filter_policy == nullptr
                        ? nullptr
                        : std::make_unique<FilterBlockBuilder>(options->filter_policy)) {}

SSTableBuilder::~SSTableBuilder() = default;

void SSTableBuilder::add(const Binary& key, const Binary& value) {
    assert(!closed_);

    if (!ok()) {
        return;
    }

    if (key_nums_ == 0) {
        first_key_.assign(key.data(), key.size());
    }

    if (pending_index_entry_) {
        assert(data_block_.empty());
        std::string handle_encoding;
        pending_handler_.encodeTo(&handle_encoding);
        // 记录上一个block的最后一个key和上一个block的结束位置
        index_block_.add(last_key_, handle_encoding);
        pending_index_entry_ = false;
    }

    if (key_nums_ > 0) {
        assert(options_->comparator->compare(key, last_key_) > 0);
    }

    if (filter_block_ != nullptr) {
        filter_block_->addKey(key);
    }

    last_key_.assign(key.data(), key.size());
    data_block_.add(key, value);
    key_nums_++;

    const size_t s = data_block_.sizeEstimate();
    // 达到block_size后，就写入
    if (s >= options_->block_size) {
        flush();
    }
}

void SSTableBuilder::flush() {
    assert(!closed_);
    if (!ok() || data_block_.empty()) {
        return;
    }
    // 写入block
    writeBlock(&data_block_, &pending_handler_);
    if (ok()) {
        // 复位标记，下次开始一个新的block
        pending_index_entry_ = true;
        status_ = writer_->flush();
    }
    if (filter_block_ != nullptr) {
        filter_block_->startBlock(offset_);
    }
}

void SSTableBuilder::writeBlock(BlockBuilder* block, BlockHandle* handle) {
    assert(ok());
    auto raw = block->finish();
    std::string compressed;
    switch (options_->compression_type) {
    case CompressionType::SNAPPY:
    {
        auto outlen = snappy::Compress(raw.data(), raw.size(), &compressed);
        compressed.resize(outlen);
        raw.reset(compressed);
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
        raw.reset(compressed);
        break;
    }
    default:
        break;
    }
    writeBlockRaw(raw, options_->compression_type, handle);
    block->reset();
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
void SSTableBuilder::writeBlockRaw(const Binary& content,
                                   CompressionType type,
                                   BlockHandle* handle) {
    handle->setOffset(offset_);
    handle->setSize(content.size());
    status_ = writer_->append(content);
    if (!ok()) {
        assert(false);
    }
    // crc
    char trailer[BLOCK_TRAILER_LEN];
    trailer[0] = static_cast<const char>(type);
    uint32_t crc = crc32(content.data(), content.size());
    std::string encode_crc;
    encodeInt<uint32_t>(&encode_crc, crc);
    memcpy(trailer + 1, encode_crc.data(), encode_crc.size());
    status_ = writer_->append(Binary(trailer, BLOCK_TRAILER_LEN));
    if (!ok()) {
        assert(false);
    }
    // 更新下一个block的offset
    offset_ += content.size() + BLOCK_TRAILER_LEN;
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
Status SSTableBuilder::finish() {
    assert(!closed_);
    flush();
    closed_ = true;
    BlockHandle filter_block_handle;
    BlockHandle index_block_handle;
    BlockHandle file_meta_handle;

    if (!ok()) {
        return status();
    }

    // 写filter block
    if (filter_block_ != nullptr) {
        writeBlockRaw(filter_block_->finish(), CompressionType::NONE, &filter_block_handle);
    }

    if (!ok()) {
        return status();
    }

    // 写入index block
    // 处理边界情况
    if (pending_index_entry_) {
        std::string handle_encoding;
        pending_handler_.encodeTo(&handle_encoding);
        // 记录上一个block的最后一个key和上一个block的结束位置
        index_block_.add(last_key_, handle_encoding);
        pending_index_entry_ = false;
    }
    writeBlock(&index_block_, &index_block_handle);

    if (!ok()) {
        return status();
    }

    // 写入file meta，使用block格式有crc校验
    FileMeta file_meta;
    file_meta.setSSTType(options_->sst_type);
    file_meta.setSSTVersion(options_->sst_version);
    file_meta.setFilterPolicyType(options_->filter_type);
    file_meta.setBitsPerKey(options_->bits_per_key);
    file_meta.setMinKey(first_key_);
    file_meta.setMaxKey(last_key_);
    file_meta.setKeyNum(key_nums_);
    file_meta.setPatchId(options_->patch_id);
    file_meta.setSSTId(options_->sst_id);
    std::string file_meta_content;
    file_meta.encodeTo(&file_meta_content);
    writeBlockRaw(file_meta_content, CompressionType::NONE, &file_meta_handle);

    // 写入footer
    Footer footer;
    footer.setFileMetaHandle(file_meta_handle);
    footer.setFilterHandle(filter_block_handle);
    footer.setIndexHandle(index_block_handle);
    std::string footer_content;
    footer.encodeTo(&footer_content);
    status_ = writer_->append(footer_content);
    if (ok()) {
        offset_ += footer_content.size();
    }
    writer_->flush();

    return status();
}

} // namespace pl
