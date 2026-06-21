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
// Created: 2026/06/13 18:35

#pragma once

#include <cstdint>

namespace pl::sstv2::types {

// =============================================================================
// OpType: the operation type for a row in the SSTable.
//
// Stored as uint8 on wire. Sorted ascending in the all-key:
//   Put(0) < Merge(1) < Delete(2)
//
// For the same RowKey+Version, Put sorts first (base value), Merge next
// (incremental update applied on top of base), Delete last (terminates
// the key — compaction can discard earlier Merge/Put below it).
// =============================================================================

// clang-format off
enum class OpType : uint8_t {
    kPut    = 0,    // Full value write (base).
    kMerge  = 1,  // Incremental update, resolved by MergeOperator during compaction/read.
    kDelete = 2, // Tombstone (logical deletion).
};
// clang-format on

} // namespace pl::sstv2::types
