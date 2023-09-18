//=====================================================================
//
// sstable_builder.cpp -
//
// Created by liubang on 2023/05/31 19:39
// Last Modified: 2023/05/31 19:39
//
//=====================================================================
#include "cpp/misc/sst/sstable_builder.h"
#include "cpp/misc/sst/encoding.h"
#include "cpp/tools/crc.h"

namespace pl {

SSTableBuilder::SSTableBuilder(const Options* options, FsWriter* writer)
    : options_(options),
      writer_(writer),
      data_block_(options),
      index_block_(options),
      filter_block_(options->filter_policy == nullptr
                        ? nullptr
                        : new FilterBlockBuilder(options->filter_policy)) {}

SSTableBuilder::~SSTableBuilder() {
    if (filter_block_ != nullptr) {
        delete filter_block_;
    }
};

void SSTableBuilder::add(const Binary& key, const Binary& value) {
    assert(!closed_);
    if (!ok()) {
        return;
    }

    if (pending_index_entry_) {
        assert(data_block_.empty());
        std::string handle_encoding;
        pending_handler_.encodeTo(&handle_encoding);
        // 记录上一个block的最后一个key和上一个block的结束位置
        index_block_.add(last_key_, handle_encoding);
        pending_index_entry_ = false;
    }

    if (num_entries_ > 0) {
        assert(options_->comparator->compare(key, last_key_) > 0);
    }

    if (filter_block_ != nullptr) {
        filter_block_->addKey(key);
    }

    last_key_.assign(key.data(), key.size());
    data_block_.add(key, value);
    num_entries_++;

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
        status_              = writer_->flush();
    }
    if (filter_block_ != nullptr) {
        filter_block_->startBlock(offset_);
    }
}

void SSTableBuilder::writeBlock(BlockBuilder* block, BlockHandle* handle) {
    assert(ok());
    auto raw = block->finish();
    // TODO(liubang): support compression
    writeBlockRaw(raw, CompressionType::kNoCompression, handle);
    block->reset();
}

/*
 *
 *
 *                                                     +---------------------+
 *                                                     |  shared size(4B)    |
 *                                                     +---------------------+
 *                                                     | non shared size(4B) |
 *                                  +----------------->+---------------------+
 *                                  |                  |   value size(4B)    |
 *                                  |                  +---------------------+
 *                                  |                  | non shared key(4B)  |
 *                                  |                  +---------------------+
 *                                  |                  |      value          |
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
    if (ok()) {
        // crc
        char trailer[kBlockTrailerSize];
        trailer[0]   = static_cast<unsigned char>(type);
        uint32_t crc = crc32(content.data(), content.size());
        std::string encode_crc;
        encodeInt<uint32_t>(&encode_crc, crc);
        memcpy(trailer + 1, encode_crc.data(), encode_crc.size());
        status_ = writer_->append(Binary(trailer, kBlockTrailerSize));
        if (ok()) {
            // 更新下一个block的offset
            offset_ += content.size() + kBlockTrailerSize;
        }
    }
}

/*
 *
 * +----------------+
 * |  filter block  |
 * +----------------+
 * | metaindex block|
 * +----------------+
 * |  index block   |
 * +----------------+
 * |   footer       |
 * +----------------+
 *
 */
Status SSTableBuilder::finish() {
    assert(!closed_);
    flush();
    closed_ = true;
    BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

    // 写filter block
    if (ok() && filter_block_ != nullptr) {
        writeBlockRaw(filter_block_->finish(), CompressionType::kNoCompression,
                      &filter_block_handle);
    }

    // 写入metaindex block
    if (ok()) {
        BlockBuilder meta_index_block(options_);
        if (filter_block_ != nullptr) {
            std::string key = "filter.";
            key.append(options_->filter_policy->name());
            std::string handle_encoding;
            filter_block_handle.encodeTo(&handle_encoding);
            // 记录filter block的类型和位置
            meta_index_block.add(key, handle_encoding);
        }
        writeBlock(&meta_index_block, &metaindex_block_handle);
    }

    // 写入index block
    if (ok()) {
        // 处理边界情况
        if (pending_index_entry_) {
            std::string handle_encoding;
            pending_handler_.encodeTo(&handle_encoding);
            // 记录上一个block的最后一个key和上一个block的结束位置
            index_block_.add(last_key_, handle_encoding);
            pending_index_entry_ = false;
        }
        writeBlock(&index_block_, &index_block_handle);
    }

    // 写入footer
    if (ok()) {
        Footer footer;
        footer.setMetaindexHandle(metaindex_block_handle);
        footer.setIndexHandle(index_block_handle);
        std::string footer_content;
        footer.encodeTo(&footer_content);
        status_ = writer_->append(footer_content);
        if (ok()) {
            offset_ += footer_content.size();
        }
    }
    writer_->flush();
    return status_;
}

} // namespace pl
