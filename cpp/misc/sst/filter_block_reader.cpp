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

#include "cpp/misc/sst/filter_block_reader.h"
#include "cpp/misc/sst/encoding.h"

namespace pl {

FilterBlockReader::FilterBlockReader(FilterPolicyRef filter_policy, const pl::Binary& contents)
    : filter_policy_(std::move(filter_policy)) {
    std::size_t n = contents.size();
    if (n < 5) {
        return;
    }
    base_lg_ = static_cast<std::size_t>(contents[n - 1]);
    auto last_word = decodeInt<uint32_t>(contents.data() + n - 5);
    if (last_word > n - 5) {
        return;
    }
    data_ = contents.data();
    offset_ = data_ + last_word;
    num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::keyMayMatch(uint64_t block_offset, const Binary& key) {
    uint64_t idx = block_offset >> base_lg_;
    if (idx >= num_) {
        return true;
    }
    auto start = decodeInt<uint32_t>(offset_ + idx * 4);
    auto limit = decodeInt<uint32_t>(offset_ + idx * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
        return filter_policy_->keyMayMatch(key, Binary(data_ + start, limit - start));
    }
    return false;
}

} // namespace pl
