// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/06/04 12:01

#pragma once

#include <cstddef>
#include <cstdint>

namespace pl::sstv2::types {

// File format identifier: ASCII "SST2"
constexpr uint32_t kSstMagic = 0x53535432;
constexpr uint16_t kFormatVersion = 1;

// Block size limits
constexpr size_t kDefaultBlockSize = 64 * 1024; // 64 KB
constexpr size_t kMinBlockSize = 4 * 1024;      // 4 KB
constexpr size_t kMaxBlockSize = 1024 * 1024;   // 1 MB

// Index configuration
constexpr size_t kDefaultIndexBlockSize = 4 * 1024; // 4 KB
constexpr size_t kMaxIndexLevels = 8;

// Bloom filter
constexpr size_t kDefaultBloomBitsPerKey = 10;

// KV separation threshold
constexpr size_t kDefaultValueSizeThreshold = 1024; // 1 KB

// Checksum
constexpr uint32_t kCrc32cSeed = 0;

// Tail size in bytes
constexpr size_t kTailSize = 32;

// Block type magic numbers for integrity verification
constexpr uint32_t kDataBlockMagic = 0x44424C4B;  // "DBLK"
constexpr uint32_t kIndexBlockMagic = 0x49424C4B; // "IBLK"

} // namespace pl::sstv2::types
