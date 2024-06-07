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

#include "cpp/misc/sst/sstable.h"
#include "cpp/misc/sst/sstable_iterator.h"

namespace pl {

SSTable::SSTable(OptionsRef options,
                 FsReaderRef reader, /*const BlockHandle& metaindex_handle,*/
                 FileMetaRef file_meta,
                 BlockRef index_block)
    : options_(std::move(options)),
      reader_(std::move(reader)),
      file_meta_(std::move(file_meta)),
      index_block_(std::move(index_block)) {}

std::unique_ptr<SSTable> SSTable::open(const OptionsRef& options,
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
    Binary file_meta_input;
    std::unique_ptr<char[]> file_meta_content =
        std::make_unique<char[]>(footer.fileMetaHandle().size());
    *status = reader->read(footer.fileMetaHandle().offset(), footer.fileMetaHandle().size(),
                           &file_meta_input, file_meta_content.get());
    if (!status->isOk()) {
        return nullptr;
    }

    auto file_meta = std::make_unique<FileMeta>();
    *status = file_meta->decodeFrom(file_meta_input);
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
    table->readMeta(footer);
    return table;
}

void SSTable::readMeta(const Footer& footer) {
    if (options_->filter_policy == nullptr) {
        assert(false);
        return;
    }

    BlockContents contents;
    auto s = BlockReader::readBlock(reader_, footer.metaIndexHandle(), &contents);
    if (!s.isOk()) {
        assert(false);
        return;
    }

    auto meta = std::make_shared<Block>(contents);
    if (!meta->valid()) {
        assert(false);
    }

    auto iter = meta->iterator(options_->comparator);
    std::string key = "filter.";
    key.append(options_->filter_policy->name());
    iter->seek(key);
    if (!iter->status().isOk()) {
        assert(false);
    }

    if (iter->valid() && iter->key() == Binary(key)) {
        readFilter(iter->val());
    }
}

void SSTable::readFilter(const Binary& filter_handle_value) {
    BlockHandle filter_handle;
    if (!filter_handle.decodeFrom(filter_handle_value).isOk()) {
        assert(false);
        return;
    }

    BlockContents block;
    if (!BlockReader::readBlock(reader_, filter_handle, &block).isOk()) {
        assert(false);
        return;
    }

    if (block.heap_allocated) {
        filter_data_.reset(block.data.data());
    }

    filter_ = std::make_unique<FilterBlockReader>(options_->filter_policy, block.data);
}

Status SSTable::get(const Binary& key, void* arg, HandleResult&& handle_result) {
    if (key.compare(file_meta_->minKey()) < 0 || key.compare(file_meta_->maxKey()) > 0) {
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
