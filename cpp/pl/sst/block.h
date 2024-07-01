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

#include "cpp/pl/sst/comparator.h"
#include "cpp/pl/sst/iterator.h"
#include "cpp/pl/sst/sstable_format.h"

#include <memory>

namespace pl {

// TODO: 这个实现不好，需要优化
class Block : public std::enable_shared_from_this<Block> {
public:
    explicit Block(const BlockContents& content);
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    virtual ~Block();

    [[nodiscard]] bool valid() const { return size_ > 0; }

    IteratorPtr iterator(const ComparatorRef& comparator);

private:
    class BlockIterator;

private:
    const char* data_{nullptr};
    std::size_t size_{0};
    std::size_t num_restarts_{0};
    uint32_t restart_offset_{0};
    bool owned_{false};
};

using BlockRef = std::shared_ptr<Block>;

} // namespace pl
