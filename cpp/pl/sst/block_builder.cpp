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

#include "cpp/pl/sst/block_builder.h"
#include "cpp/pl/sst/encoding.h"

#include <cassert>

namespace pl {

BlockBuilder::BlockBuilder(const BuildOptionsRef& options)
    : comparator_(options->comparator), block_restart_interval_(options->block_restart_interval) {
    restarts_.push_back(0);
}

void BlockBuilder::add(const Cell& cell) {
    assert(!finished_);
    auto last_key_pice = std::string_view(last_key_.data(), last_rowkey_len_);
    auto rowkey = cell.rowkey();
    auto cellkey = cell.cellKey().encode();
    auto value = cell.value();
    // 必须保证rowkey按照指定的comparator的递增的顺序
    assert(buffer_.empty() || comparator_->compare(rowkey, last_key_pice) >= 0);
    uint32_t shared = 0;
    if (counter_ < block_restart_interval_) {
        // 计算当前rowkey和前一个rowkey的最大公共前缀
        const uint32_t min_length = std::min(last_key_pice.size(), rowkey.size());
        while (shared < min_length && (last_key_pice[shared] == rowkey[shared])) {
            shared++;
        }
    } else {
        restarts_.push_back(buffer_.size());
        counter_ = 0;
    }

    // clang-format off
    /**
     * +-------------+----------------+-----------------+----------------+----------------+-------+
     * | shared (4B) | non shared(4B) | rowkey size(4B) | value size(4B) | non shared key | value |
     * +-------------+----------------+-----------------+----------------+----------------+-------+
     *
     */
    // clang-format on

    // NOTE(liubang): 这里计算公共前缀的时候，只考虑了rowkey，但是计算non shared的时候，需要将整个
    // cellkey都考虑进去，后面写入non shared key的时候，是整个cellkey去掉rowkey shared的部分后的内容
    const uint32_t non_shared = cellkey.size() - shared;
    encodeInt<uint32_t>(&buffer_, shared);
    encodeInt<uint32_t>(&buffer_, non_shared);
    encodeInt<uint32_t>(&buffer_, static_cast<uint32_t>(rowkey.size()));
    encodeInt<uint32_t>(&buffer_, static_cast<uint32_t>(value.size()));

    // LOG_DEBUG << "shared:" << shared << ", non_shared:" << non_shared
    //           << ", rowkey:" << rowkey.size() << ", value:" << value.size();

    buffer_.append(cellkey.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());

    last_key_.resize(shared);
    last_key_.append(cellkey.data() + shared, non_shared);
    last_rowkey_len_ = rowkey.size();
    assert(std::string_view(last_key_).compare(cellkey) == 0);
    counter_++;
}

std::string_view BlockBuilder::finish() {
    /*
     * restarts indexes
     *
     * +---------------+---------------+--------+---------------+------------------+
     * | restart[0] 4B | restart[1] 4B | ...... | restart[n] 4B | restart count 4B |
     * +---------------+---------------+--------+---------------+------------------+
     */
    for (uint32_t restart : restarts_) {
        encodeInt<uint32_t>(&buffer_, restart);
    }

    encodeInt<uint32_t>(&buffer_, static_cast<uint32_t>(restarts_.size()));
    finished_ = true;
    return {buffer_};
}

std::size_t BlockBuilder::sizeEstimate() const {
    return (buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t));
}

void BlockBuilder::reset() {
    buffer_.clear();
    last_key_.clear();
    counter_ = 0;
    finished_ = false;
    restarts_.clear();
    restarts_.push_back(0);
}

} // namespace pl
