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

#pragma once

#include "cpp/pl/sst/iterator.h"

#include <functional>

namespace pl {

using BlockFunc = std::function<IteratorPtr(const Binary)>;

class SSTableIterator : public Iterator {
public:
    SSTableIterator(IteratorPtr index_iter, BlockFunc block_func)
        : index_iter_(std::move(index_iter)), block_func_(std::move(block_func)) {}

    ~SSTableIterator() override = default;

    void first() override;

    void last() override;

    void next() override;

    void prev() override;

    void seek(const Binary& target) override;

    [[nodiscard]] Status status() const override;

    [[nodiscard]] bool valid() const override;

    [[nodiscard]] Binary key() const override;

    [[nodiscard]] Binary val() const override;

private:
    void initDataBlock();
    void forwardSkipEmptyData();
    void backwardSkipEmptyData();

private:
    Status status_;
    IteratorPtr index_iter_;
    IteratorPtr data_iter_;
    std::string data_block_handle_;
    BlockFunc block_func_;
};

} // namespace pl
