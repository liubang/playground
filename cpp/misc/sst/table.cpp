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

#include "cpp/misc/sst/table.h"

#include <memory>

namespace pl {

Table::Table(const OptionsRef& options,
             const FsReaderRef& reader, /*const BlockHandle& metaindex_handle,*/
             BlockPtr index_block)
    : options_(options), reader_(reader), index_block_(std::move(index_block)) {}

std::unique_ptr<Table> Table::open(const OptionsRef& options,
                                   const FsReaderRef& reader,
                                   uint64_t size,
                                   Status* status) {
    if (size < Footer::kEncodedLength) {
        *status = Status::NewCorruption("file is too short to be an sstable");
        return nullptr;
    }
    char footer_content[Footer::kEncodedLength];
    Binary footer_input;
    *status = reader->read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                           footer_content);
    if (!status->isOk()) {
        return nullptr;
    }
    Footer footer;
    *status = footer.decodeFrom(footer_input);
    if (!status->isOk()) {
        return nullptr;
    }

    // parse index block
    BlockContents index_block_contents;
    *status = BlockReader::readBlock(reader, footer.indexHandle(), &index_block_contents);
    if (!status->isOk()) {
        return nullptr;
    }

    auto index_block = std::make_unique<Block>(index_block_contents);

    auto table =
        std::unique_ptr<Table>(new Table(options, reader,
                                         /* footer.metaindexHandle(), */ std::move(index_block)));
    table->readMeta(footer);
    return table;
}

void Table::readMeta(const Footer& footer) {
    if (options_->filter_policy == nullptr) {
        return;
    }

    BlockContents contents;
    auto s = BlockReader::readBlock(reader_, footer.metaIndexHandle(), &contents);
    if (!s.isOk()) {
        return;
    }

    auto meta = std::make_unique<Block>(contents);
    auto* iter = meta->iterator(options_->comparator);

    std::string key = "filter.";
    key.append(options_->filter_policy->name());
    iter->seek(key);
    if (iter->valid() && iter->key() == Binary(key)) {
        readFilter(iter->val());
    }
    delete iter;
}

void Table::readFilter(const Binary& filter_handle_value) {
    BlockHandle filter_handle;
    if (!filter_handle.decodeFrom(filter_handle_value).isOk()) {
        return;
    }

    BlockContents block;
    if (!BlockReader::readBlock(reader_, filter_handle, &block).isOk()) {
        return;
    }

    if (block.heap_allocated) {
        filter_data_.reset(block.data.data());
    }

    filter_ = std::make_unique<FilterBlockReader>(options_->filter_policy, block.data);
}

Status Table::get(const Binary& key, void* arg, HandleResult&& handle_result) {
    Status s;
    auto* iiter = index_block_->iterator(options_->comparator);
    iiter->seek(key);
    if (iiter->valid()) {
        Binary handle_value = iiter->val();
        BlockHandle handle;
        if (filter_ != nullptr && handle.decodeFrom(handle_value).isOk() &&
            !filter_->keyMayMatch(handle.offset(), key)) {
            // not found
            s = Status::NewNotFound();
        } else {
            // key may found
            auto* iter = blockReader(iiter->val());
            if (nullptr == iter) {
                delete iiter;
                return Status::NewNotFound();
            }
            iter->seek(key);
            if (iter->valid()) {
                handle_result(arg, key, iter->val());
            }
            s = iter->status();
            delete iter;
        }
    }

    delete iiter;
    return s;
}

Iterator* Table::blockReader(const Binary& index_value) {
    Block* block = nullptr;
    BlockHandle handle;
    auto s = handle.decodeFrom(index_value);
    if (s.isOk()) {
        BlockContents contents;
        auto s = BlockReader::readBlock(reader_, handle, &contents);
        if (s.isOk()) {
            block = new Block(contents);
        }
    }

    Iterator* iter = nullptr;
    if (nullptr != block) {
        iter = block->iterator(options_->comparator);
        iter->registerCleanup([block]() {
            delete block;
        });
    }

    return iter;
}

} // namespace pl
