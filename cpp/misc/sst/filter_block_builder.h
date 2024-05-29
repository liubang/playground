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

#include <string>
#include <vector>

#include "cpp/misc/sst/filter_policy.h"
#include "cpp/tools/binary.h"

namespace pl {

class FilterBlockBuilder {
public:
    explicit FilterBlockBuilder(const FilterPolicy* filter_policy);

    FilterBlockBuilder(const FilterBlockBuilder&) = delete;

    FilterPolicy& operator=(const FilterBlockBuilder&) = delete;

    void startBlock(uint64_t offset);

    void addKey(const Binary& key);

    Binary finish();

private:
    void genFilter();

private:
    const FilterPolicy* filter_policy_;
    std::string keys_;                     // 所有的key组合成一个大的string
    std::vector<std::size_t> start_;       // 每一个key在keys_中的偏移,可以通过start_[i
                                           // + 1] - start_[i] 计算出第i个key的长度
    std::string result_;                   // 所有的filter都存放在result_中
    std::vector<uint32_t> filter_offsets_; // 每个filter在result_中的offset
    std::vector<Binary> tmp_keys_;         // 生成新的filter的时候存储临时的key
};

} // namespace pl
