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

#include "cpp/pl/sst/sstable.h"
#include "cpp/pl/fs/posix_fs.h"
#include "cpp/pl/sst/encoding.h"
#include "cpp/pl/sst/sstable_iterator.h"

#include <folly/ScopeGuard.h>

namespace pl {

SSTable::SSTable(ReadOptionsRef options,
                 FileDescriptorRef fd,
                 FileSystemRef reader,
                 FileMetaRef file_meta,
                 BlockRef index_block)
    : options_(std::move(options)),
      fd_(std::move(fd)),
      reader_(std::move(reader)),
      file_meta_(std::move(file_meta)),
      index_block_(std::move(index_block)) {}

Result<SSTablePtr> SSTable::open(const ReadOptionsRef& options,
                                 const std::filesystem::path& sst_file) {
    FileSystemRef fs = std::make_shared<PosixFileSystem>();
    FileDescriptorRef fd;
    auto result = fs->open(sst_file.string(), 0, &fd);
    RETURN_AND_LOG_ON_ERROR(result);

    uint64_t size;
    result = fs->size(fd, &size);
    RETURN_AND_LOG_ON_ERROR(result);

    // check footer
    if (size < FOOTER_LEN) {
        return makeError(StatusCode::kDataCorruption, "file is too short to be an sstable");
    }

    char footer_content[FOOTER_LEN];
    std::string_view footer_input;
    result = fs->pread(fd, size - FOOTER_LEN, FOOTER_LEN, footer_content, &footer_input);
    RETURN_AND_LOG_ON_ERROR(result);

    Footer footer;
    result = footer.decodeFrom(footer_input);
    RETURN_AND_LOG_ON_ERROR(result);

    // file meta
    BlockContents file_meta_content;
    auto file_block_result = BlockReader::readBlock(fs, fd, footer.fileMetaHandle());
    RETURN_AND_LOG_ON_ERROR(file_block_result);
    file_meta_content = file_block_result.value();

    SCOPE_EXIT {
        if (file_meta_content.heap_allocated) {
            delete[] file_meta_content.data.data();
        }
    };
    auto file_meta = std::make_unique<FileMeta>();
    result = file_meta->decodeFrom(file_meta_content.data);
    RETURN_AND_LOG_ON_ERROR(result);

    // parse index block
    BlockContents index_block_contents;
    auto index_block_result = BlockReader::readBlock(fs, fd, footer.indexHandle());
    RETURN_AND_LOG_ON_ERROR(index_block_result);
    index_block_contents = index_block_result.value();

    auto index_block = std::make_shared<Block>(index_block_contents);
    auto table = std::unique_ptr<SSTable>(
        new SSTable(options, fd, fs, std::move(file_meta), std::move(index_block)));

    table->readFilter(footer);
    return table;
}

Result<Void> SSTable::readFilter(const Footer& footer) {
    auto filter_type = file_meta_->filterPolicyType();
    if (filter_type == FilterPolicyType::NONE) {
        RETURN_VOID;
    }
    BlockHandle filter_handle = footer.filterHandle();
    auto filter_block_result = BlockReader::readBlock(reader_, fd_, filter_handle);
    RETURN_AND_LOG_ON_ERROR(filter_block_result);

    BlockContents block = filter_block_result.value();

    if (block.heap_allocated) {
        filter_data_.reset(block.data.data());
    }

    uint8_t num_probes = decodeInt<uint8_t>(block.data.data() + (block.data.size() - 1));

    switch (filter_type) {
    case FilterPolicyType::STANDARD_BLOOM_FILTER:
        filter_ = std::make_unique<StandardBloomFilterReader>(block.data.data(), block.data.size(),
                                                              num_probes);
        break;
    case pl::FilterPolicyType::BLOCKED_BLOOM_FILTER:
        filter_ = std::make_unique<BlockedBloomFilterReader>(block.data.data(), block.data.size(),
                                                             num_probes);
        break;
    default:
        assert(false);
    }

    RETURN_VOID;
}

IteratorPtr SSTable::blockReader(std::string_view index_value) {
    BlockHandle handle;
    auto s = handle.decodeFrom(index_value);
    if (s.hasError()) {
        XLOGF(ERR, "decode block index error: {}", s.error());
        return nullptr;
    }

    auto block_content_result = BlockReader::readBlock(reader_, fd_, handle);

    if (block_content_result.hasError()) {
        XLOGF(ERR, "read block error: {}", block_content_result.error());
        return nullptr;
    }

    BlockContents contents = block_content_result.value();
    auto block = std::make_shared<Block>(contents);
    auto iter = block->iterator(options_->comparator);

    return iter;
}

// 将来会废弃这个接口，在上层统一实现query和scan操作语义
Result<CellVecRef> SSTable::get(std::string_view rowkey, Arena* buf) {
    auto iiter = index_block_->iterator(options_->comparator);
    iiter->seek(rowkey);
    if (!iiter->valid()) {
        return makeError(iiter->status());
    }
    auto idx_cell = iiter->cell();
    assert(idx_cell != nullptr);
    auto idx_handle = idx_cell->value();
    if (filter_ != nullptr) {
        BlockHandle handle;
        auto result = handle.decodeFrom(idx_handle);
        RETURN_AND_LOG_ON_ERROR(result);

        if (!filter_->key_may_match(rowkey)) {
            return makeError(StatusCode::kKVStoreNotFound);
        }
    }
    auto data_iter = blockReader(idx_handle);
    if (data_iter == nullptr) {
        return makeError(StatusCode::kDataCorruption, "invalid data block");
    }
    data_iter->seek(rowkey);
    if (!data_iter->valid()) {
        return makeError(StatusCode::kKVStoreNotFound);
    }

    // first cell
    auto cell = data_iter->cell();
    if (options_->comparator->compare(cell->rowkey(), rowkey) != 0) {
        return makeError(StatusCode::kKVStoreNotFound);
    }

    CellVecRef cells;

    // should copy
    cells.emplace_back(cell->clone(buf));
    data_iter->next();

    // get all cells of the row
    // TODO(liubang): cell merger
    while (data_iter->valid()) {
        cell = data_iter->cell();
        if (options_->comparator->compare(cell->rowkey(), rowkey) != 0) {
            break;
        }
        cells.emplace_back(cell->clone(buf));
        data_iter->next();
    }

    return std::move(cells);
}

IteratorPtr SSTable::iterator() {
    return std::make_unique<SSTableIterator>(index_block_->iterator(options_->comparator),
                                             [that = this](std::string_view b) {
                                                 return that->blockReader(b);
                                             });
}

} // namespace pl
