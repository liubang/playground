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

#include "cpp/misc/sst/filter_policy.h"
#include "cpp/tools/binary.h"

namespace pl {

class FilterBlockReader {
public:
    FilterBlockReader(FilterPolicyRef filter_policy, const pl::Binary& contents);

    bool keyMayMatch(uint64_t block_offset, const Binary& key);

private:
    const FilterPolicyRef filter_policy_; // filter 策略
    const char* data_;                    //
    const char* offset_;
    size_t num_;
    size_t base_lg_;
};

using FilterBlockReaderPtr = std::unique_ptr<FilterBlockReader>;
using FilterBlockReaderRef = std::shared_ptr<FilterBlockReader>;

} // namespace pl
