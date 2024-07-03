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
#include "cpp/pl/log/logger.h"
#include "cpp/pl/scope/scope.h"
#include "cpp/pl/sst/sstable_iterator.h"

namespace pl {

SSTable::SSTable(ReadOptionsRef options,
                 FsReaderRef reader,
                 FileMetaRef file_meta,
                 BlockRef index_block)
    : options_(std::move(options)),
      reader_(std::move(reader)),
      file_meta_(std::move(file_meta)),
      index_block_(std::move(index_block)) {}

std::unique_ptr<SSTable> SSTable::open(const ReadOptionsRef& options,
                                       const FsReaderRef& reader,
                                       uint64_t size,
                                       Status* status) {
    if (size < FOOTER_LEN) {
        *status = Status::NewCorruption("file is too short to be an sstable");
        return nullptr;
    }
    char footer_content[FOOTER_LEN];
    std::string_view footer_input;
    *status = reader->read(size - FOOTER_LEN, FOOTER_LEN, &footer_input, footer_content);
    if (!status->isOk()) {
        return nullptr;
    }

    Footer footer;
    *status = footer.decodeFrom(footer_input);
    if (!status->isOk()) {
        return nullptr;
    }

    // file meta
    BlockContents file_meta_content;
    *status = BlockReader::readBlock(reader, footer.fileMetaHandle(), &file_meta_content);
    if (!status->isOk()) {
        return nullptr;
    }

    SCOPE_EXIT {
        if (file_meta_content.heap_allocated) {
            delete[] file_meta_content.data.data();
        }
    };
    auto file_meta = std::make_unique<FileMeta>();
    *status = file_meta->decodeFrom(file_meta_content.data);
    if (!status->isOk()) {
        return nullptr;
    }

    // parse index block
    BlockContents index_block_contents;
    *status = BlockReader::readBlock(reader, footer.indexHandle(), &index_block_contents);
    if (!status->isOk()) {
        return nullptr;
    }

    auto index_block = std::make_shared<Block>(index_block_contents);
    auto table = std::unique_ptr<SSTable>(
        new SSTable(options, reader, std::move(file_meta), std::move(index_block)));

    table->readFilter(footer);
    return table;
}

void SSTable::readFilter(const Footer& footer) {
    auto filter_type = file_meta_->filterPolicyType();
    if (filter_type == FilterPolicyType::NONE) {
        return;
    }
    BlockHandle filter_handle = footer.filterHandle();
    BlockContents block;
    auto st = BlockReader::readBlock(reader_, filter_handle, &block);
    if (!st.isOk()) {
        LOG_ERROR << "read block error: " << st.msg();
        return;
    }

    if (block.heap_allocated) {
        filter_data_.reset(block.data.data());
    }

    FilterPolicyRef filter = nullptr;
    switch (filter_type) {
    case FilterPolicyType::BLOOM_FILTER:
        filter = std::make_shared<BloomFilterPolicy>(file_meta_->bitsPerKey());
        break;
    default:
        assert(false);
    }
    assert(filter != nullptr);

    filter_ = std::make_shared<FilterBlockReader>(std::move(filter), block.data);
}

IteratorPtr SSTable::blockReader(std::string_view index_value) {
    BlockHandle handle;
    auto s = handle.decodeFrom(index_value);
    if (!s.isOk()) {
        LOG_ERROR << "decode block index error: " << s.msg();
        return nullptr;
    }

    BlockContents contents;
    s = BlockReader::readBlock(reader_, handle, &contents);

    if (!s.isOk()) {
        LOG_ERROR << "read block error: " << s.msg();
        return nullptr;
    }

    auto block = std::make_shared<Block>(contents);
    auto iter = block->iterator(options_->comparator);

    return iter;
}

// 将来会废弃这个接口，在上层统一实现query和scan操作语义
Status SSTable::get(std::string_view rowkey, Arena* buf, CellVecRef* cells) {
    auto iiter = index_block_->iterator(options_->comparator);
    iiter->seek(rowkey);
    if (!iiter->valid()) {
        return iiter->status();
    }
    Status st = Status::NewOk();
    auto idx_cell = iiter->cell();
    assert(idx_cell != nullptr);
    auto idx_handle = idx_cell->value();
    if (filter_ != nullptr) {
        BlockHandle handle;
        st = handle.decodeFrom(idx_handle);
        if (!st.isOk()) {
            return st;
        }
        if (!filter_->keyMayMatch(handle.offset(), rowkey)) {
            st = Status::NewNotFound();
            // LOG_DEBUG << "key not found, rowkey:" << rowkey;
            return st;
        }
    }
    auto data_iter = blockReader(idx_handle);
    if (data_iter == nullptr) {
        st = Status::NewCorruption("invalid data block");
        return st;
    }
    data_iter->seek(rowkey);
    if (!data_iter->valid()) {
        st = Status::NewNotFound();
        return st;
    }

    // first cell
    auto cell = data_iter->cell();
    if (options_->comparator->compare(cell->rowkey(), rowkey) != 0) {
        st = Status::NewNotFound();
        return st;
    }

    // should copy
    cells->emplace_back(cell->clone(buf));
    data_iter->next();

    // get all cells of the row
    // TODO(liubang): cell merger
    while (data_iter->valid()) {
        cell = data_iter->cell();
        if (options_->comparator->compare(cell->rowkey(), rowkey) != 0) {
            break;
        }
        cells->emplace_back(cell->clone(buf));
        data_iter->next();
    }

    return st;
}

IteratorPtr SSTable::iterator() {
    return std::make_unique<SSTableIterator>(index_block_->iterator(options_->comparator), filter_,
                                             [that = this](std::string_view b) {
                                                 return that->blockReader(b);
                                             });
}

} // namespace pl
