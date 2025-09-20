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
#include "cpp/pl/sst/sstable_format.h"
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pl {

struct ReadOptions {
    ReadOptions() : comparator(std::make_shared<BytewiseComparator>()) {}
    const ComparatorRef comparator;
};

using ReadOptionsPtr = std::unique_ptr<ReadOptions>;
using ReadOptionsRef = std::shared_ptr<ReadOptions>;

struct BuildOptions {
    BuildOptions() : comparator(std::make_shared<BytewiseComparator>()) {}

    // 4 KB
    std::filesystem::path data_dir;
    std::size_t block_size = 4 * 1024;
    int block_restart_interval = 16;
    uint32_t bits_per_key = 10;
    CompressionType compression_type = CompressionType::NONE;
    int zstd_compress_level = 1;
    const ComparatorRef comparator;
    SSTType sst_type = SSTType::NONE;
    SSTVersion sst_version = SSTVersion::NONE;
    FilterPolicyType filter_type = FilterPolicyType::STANDARD_BLOOM_FILTER;
    PatchId patch_id = 0;
    SSTId sst_id = 0;
};

using BuildOptionsPtr = std::unique_ptr<BuildOptions>;
using BuildOptionsRef = std::shared_ptr<BuildOptions>;

} // namespace pl
