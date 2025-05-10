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

#include "cpp/pl/sst/filter_policy.h"

namespace pl {

/**
 * @class FilterBlockReader
 * @brief [TODO:description]
 * @deprecated
 */
class FilterBlockReader {
public:
    FilterBlockReader(FilterPolicyRef filter_policy, std::string_view contents);

    /**
     * @brief [TODO:description]
     *
     * @param block_offset [TODO:parameter]
     * @param key [TODO:parameter]
     * @return [TODO:return]
     *
     * @deprecated
     */
    bool keyMayMatch(uint64_t block_offset, std::string_view key);

private:
    const FilterPolicyRef filter_policy_; // filter 策略
    const char* data_;                    // filter block raw data
    const char* offset_;                  // the start of offset array
    size_t num_;                          // the count of filters
    size_t base_lg_;                      // the logarithmic for calculate size of every filter
};

// @deprecated
using FilterBlockReaderPtr = std::unique_ptr<FilterBlockReader>;
// @deprecated
using FilterBlockReaderRef = std::shared_ptr<FilterBlockReader>;

} // namespace pl
