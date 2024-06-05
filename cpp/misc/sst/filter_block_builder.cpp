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

#include "cpp/misc/sst/filter_block_builder.h"
#include "cpp/misc/sst/encoding.h"

#include <cassert>

namespace pl {

static const size_t kFilterBaseLg = 11;
constexpr static std::size_t kFilterBase = 1 << kFilterBaseLg; // 2048

FilterBlockBuilder::FilterBlockBuilder(FilterPolicyRef policy)
    : filter_policy_(std::move(policy)) {}

void FilterBlockBuilder::startBlock(uint64_t offset) {
    uint64_t filter_index = (offset / kFilterBase);
    assert(filter_index >= filter_offsets_.size());
    while (filter_index > filter_offsets_.size()) {
        genFilter();
    }
}

void FilterBlockBuilder::addKey(const Binary& key) {
    // make a copy
    Binary k = key;
    // 这两行的先后顺序不能颠倒
    start_.push_back(keys_.size());
    keys_.append(k.data(), k.size());
}

Binary FilterBlockBuilder::finish() {
    if (!start_.empty()) {
        genFilter();
    }

    const uint32_t array_offset = result_.size();
    // 将每个filter的offset写入filter block中
    for (uint32_t filter_offset : filter_offsets_) {
        encodeInt(&result_, filter_offset);
    }

    // 记录filter
    // offset的地址偏移量，通过这个偏移量，可以找到哪一段是filter_offsets
    encodeInt(&result_, array_offset);
    result_.push_back(kFilterBaseLg);
    return {result_};
}

void FilterBlockBuilder::genFilter() {
    const std::size_t num_keys = start_.size();
    if (num_keys == 0) {
        filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
        return;
    }
    start_.push_back(keys_.size());
    tmp_keys_.resize(num_keys);
    for (std::size_t i = 0; i < num_keys; i++) {
        const char* base = keys_.data() + start_[i];
        std::size_t len = start_[i + 1] - start_[i];
        tmp_keys_[i] = Binary(base, len);
    }
    filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
    filter_policy_->createFilter(&tmp_keys_[0], num_keys, &result_);

    tmp_keys_.clear();
    keys_.clear();
    start_.clear();
}

} // namespace pl
