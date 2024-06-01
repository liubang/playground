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

#include "cpp/misc/sst/comparator.h"
#include "cpp/misc/sst/filter_policy.h"

#include <cstdint>
#include <memory>

namespace pl {

enum class CompressionType : uint8_t {
    kNoCompression = 0,
    kSnappyCompression = 1,
    kZstdCompression = 2,
};

struct Options {
    Options()
        : comparator(std::make_shared<BytewiseComparator>()),
          filter_policy(std::make_shared<BloomFilterPolicy>(bits_per_key)) {}

    // 4 KB
    std::size_t block_size = 4 * 1024;

    int block_restart_interval = 16;

    uint64_t bits_per_key = 10;

    const ComparatorRef comparator;

    const FilterPolicyRef filter_policy;
};

using OptionsPtr = std::unique_ptr<Options>;
using OptionsRef = std::shared_ptr<Options>;

} // namespace pl
