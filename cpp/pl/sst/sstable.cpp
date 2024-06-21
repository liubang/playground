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
    Binary footer_input;
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

    // for RAII
    auto meta_block = std::make_shared<Block>(file_meta_content);
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
    if (!BlockReader::readBlock(reader_, filter_handle, &block).isOk()) {
        assert(false);
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

    filter_ = std::make_unique<FilterBlockReader>(std::move(filter), block.data);
}

Status SSTable::get(const Binary& key, void* arg, HandleResult&& handle_result) {
    if (options_->comparator->compare(key, file_meta_->minKey()) < 0 ||
        options_->comparator->compare(key, file_meta_->maxKey()) > 0) {
        return Status::NewNotFound();
    }

    auto iiter = index_block_->iterator(options_->comparator);
    iiter->seek(key);
    if (!iiter->valid()) {
        return Status::NewNotFound();
    }

    Binary handle_value = iiter->val();
    BlockHandle handle;
    if (filter_ != nullptr) {
        if (!handle.decodeFrom(handle_value).isOk()) {
            assert(false);
        }
        // 通过block的offset来快速查找filter的位置
        if (!filter_->keyMayMatch(handle.offset(), key)) {
            // key not found
            return Status::NewNotFound();
        }
    }
    // key may found
    auto iter = blockReader(iiter->val());
    if (nullptr == iter) {
        return Status::NewNotFound();
    }

    iter->seek(key);
    if (iter->valid()) {
        handle_result(arg, key, iter->val());
    }

    return iter->status();
}

IteratorPtr SSTable::blockReader(const Binary& index_value) {
    BlockHandle handle;
    auto s = handle.decodeFrom(index_value);
    if (!s.isOk()) {
        assert(false);
    }

    BlockContents contents;
    s = BlockReader::readBlock(reader_, handle, &contents);

    if (!s.isOk()) {
        assert(false);
    }

    auto block = std::make_shared<Block>(contents);
    auto iter = block->iterator(options_->comparator);

    return iter;
}

IteratorPtr SSTable::iterator() {
    return std::make_unique<SSTableIterator>(index_block_->iterator(options_->comparator),
                                             [this](const Binary b) {
                                                 return this->blockReader(b);
                                             });
}

} // namespace pl
