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

#include "cpp/pl/sst/sstable_iterator.h"

#include <cassert>

namespace pl {

void SSTableIterator::seek(std::string_view target) {
    index_iter_->seek(target);
    initDataBlock();
    if (data_iter_ != nullptr) {
        data_iter_->seek(target);
    }
    forwardSkipEmptyData();
}

void SSTableIterator::first() {
    index_iter_->first();
    initDataBlock();
    if (data_iter_ != nullptr) {
        data_iter_->first();
    }
    forwardSkipEmptyData();
}

void SSTableIterator::last() {
    index_iter_->last();
    initDataBlock();
    if (data_iter_ != nullptr) {
        data_iter_->last();
    }
    backwardSkipEmptyData();
}

void SSTableIterator::next() {
    assert(valid());
    data_iter_->next();
    forwardSkipEmptyData();
}

void SSTableIterator::prev() {
    assert(valid());
    data_iter_->prev();
    backwardSkipEmptyData();
}

bool SSTableIterator::valid() const { return data_iter_ != nullptr && data_iter_->valid(); }

CellRef SSTableIterator::cell() const {
    assert(valid());
    return data_iter_->cell();
}

Status SSTableIterator::status() const {
    if (!index_iter_->status().isOk()) {
        return index_iter_->status();
    }
    if (data_iter_ != nullptr && !data_iter_->status().isOk()) {
        return data_iter_->status();
    }
    return status_;
}

void SSTableIterator::initDataBlock() {
    if (!index_iter_->valid()) {
        data_iter_ = nullptr;
        return;
    }
    auto idx_cell = index_iter_->cell();
    assert(idx_cell != nullptr);
    std::string_view handle = idx_cell->value();
    if (data_iter_ != nullptr && handle.compare(data_block_handle_) == 0) {
        // do nothing
    } else {
        data_iter_ = data_block_func_(handle);
        data_block_handle_.assign(handle.data(), handle.size());
    }
}

void SSTableIterator::forwardSkipEmptyData() {
    while (data_iter_ == nullptr || !data_iter_->valid()) {
        if (!index_iter_->valid()) {
            data_iter_ = nullptr;
            return;
        }
        index_iter_->next();
        initDataBlock();
        if (data_iter_ != nullptr) {
            data_iter_->first();
        }
    }
}

void SSTableIterator::backwardSkipEmptyData() {
    while (data_iter_ == nullptr || !data_iter_->valid()) {
        if (!index_iter_->valid()) {
            data_iter_ = nullptr;
            return;
        }
        index_iter_->prev();
        initDataBlock();
        if (data_iter_ != nullptr) {
            data_iter_->last();
        }
    }
}

} // namespace pl
