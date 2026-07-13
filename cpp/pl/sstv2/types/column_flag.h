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
// Created: 2026/06/05 00:23

#pragma once

#include <cstdint>

#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::types {

// =============================================================================
// ColumnFlag: the 64-bit flag field in the Internal Table (design.md §7.1).
//
// Bit layout (low 10 bits defined, bits 10-63 reserved = 0):
//
//   Bits 0-7  [DT]  DataType enum value (0-255)
//   Bit  8    [C]   Checksum present: 1 = CRC32C computed, 0 = no checksum
//   Bit  9    [B]   Bool value: when DT=Bool, stores the boolean (1=true, 0=false)
//                   When DT≠Bool, must be 0.
//
// In index block entries, DT uses private types (21=DataBlock, 22=IndexBlock),
// and both C and B bits must be 0.
// =============================================================================

class ColumnFlag {
public:
    // Bit field constants.
    static constexpr uint64_t kDtMask = 0xFF;                     // bits 0-7
    static constexpr uint64_t kChecksumBit = 1ULL << 8;           // bit 8
    static constexpr uint64_t kBoolBit = 1ULL << 9;               // bit 9
    static constexpr uint64_t kReservedMask = ~uint64_t{0} << 10; // bits 10-63

    // =========================================================================
    // Construction.
    // =========================================================================

    constexpr ColumnFlag() = default;

    // Construct a data flag for a value column.
    static constexpr ColumnFlag for_value(DataType dt, bool checksum, bool bool_val = false) {
        uint64_t bits = static_cast<uint8_t>(dt);
        if (checksum) {
            bits |= kChecksumBit;
        }
        if (bool_val) {
            bits |= kBoolBit;
        }
        return ColumnFlag{bits};
    }

    // Construct a flag for an index entry pointing to a DataBlock.
    static constexpr ColumnFlag for_data_block() {
        return ColumnFlag{static_cast<uint64_t>(static_cast<uint8_t>(DataType::kDataBlock))};
    }

    // Construct a flag for an index entry pointing to an IndexBlock.
    static constexpr ColumnFlag for_index_block() {
        return ColumnFlag{static_cast<uint64_t>(static_cast<uint8_t>(DataType::kIndexBlock))};
    }

    // Reconstruct from raw wire representation.
    static constexpr ColumnFlag from_raw(uint64_t raw) { return ColumnFlag{raw}; }

    // =========================================================================
    // Bit field accessors.
    // =========================================================================

    [[nodiscard]] constexpr DataType data_type() const {
        return static_cast<DataType>(bits_ & kDtMask);
    }

    [[nodiscard]] constexpr bool has_checksum() const { return (bits_ & kChecksumBit) != 0; }
    [[nodiscard]] constexpr bool bool_value() const { return (bits_ & kBoolBit) != 0; }

    // =========================================================================
    // Semantic queries.
    // =========================================================================

    // True if this flag represents an index entry (DataBlock or IndexBlock pointer).
    [[nodiscard]] constexpr bool is_index_entry() const { return is_private_type(data_type()); }

    // True if this flag points to a DataBlock (leaf of index tree).
    [[nodiscard]] constexpr bool is_data_block_ptr() const {
        return data_type() == DataType::kDataBlock;
    }

    // True if this flag points to an IndexBlock (internal node of index tree).
    [[nodiscard]] constexpr bool is_index_block_ptr() const {
        return data_type() == DataType::kIndexBlock;
    }

    // True if this flag represents a user value column (not an index entry).
    [[nodiscard]] constexpr bool is_value_flag() const { return !is_index_entry(); }

    // Validate invariants:
    // - Reserved bits must be zero.
    // - Index entries must have C=0 and B=0.
    // - B bit must be 0 when DT != Bool.
    [[nodiscard]] constexpr bool is_valid() const {
        if ((bits_ & kReservedMask) != 0 || !is_valid_data_type(data_type())) {
            return false;
        }
        if (is_index_entry() && (bits_ & (kChecksumBit | kBoolBit)) != 0) {
            return false;
        }
        if (data_type() != DataType::kBool && bool_value()) {
            return false;
        }
        return true;
    }

    // =========================================================================
    // Raw access and comparison.
    // =========================================================================

    [[nodiscard]] constexpr uint64_t raw() const { return bits_; }

    constexpr bool operator==(const ColumnFlag& rhs) const { return bits_ == rhs.bits_; }
    constexpr bool operator!=(const ColumnFlag& rhs) const { return bits_ != rhs.bits_; }

private:
    constexpr explicit ColumnFlag(uint64_t bits) : bits_(bits) {}

    uint64_t bits_ = 0;
};

} // namespace pl::sstv2::types
