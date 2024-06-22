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

#include "cpp/pl/sst/filter_policy.h"
#include "cpp/pl/bloom/bloom_filter.h"

namespace pl {

void BloomFilterPolicy::createFilter(const std::vector<std::string_view>& keys,
                                     std::string* dst) const {
    BloomFilter bloom_filter(bits_per_key_);
    bloom_filter.create(keys, dst);
}

bool BloomFilterPolicy::keyMayMatch(std::string_view key, std::string_view filter) const {
    BloomFilter bloom_filter(bits_per_key_);
    return bloom_filter.contains(key, filter);
}

} // namespace pl
