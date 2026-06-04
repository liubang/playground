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

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::types {

// Type-safe value container that can hold any DataType value.
// Uses a tagged union internally with std::string as backing store
// for variable-length types (string and binary).
class Variant {
public:
    ~Variant();
    Variant(const Variant& other);
    Variant& operator=(const Variant& other);
    Variant(Variant&& other) noexcept;
    Variant& operator=(Variant&& other) noexcept;

    // === Factory methods ===
    static Variant none();
    static Variant boolean(bool v);
    static Variant int8(int8_t v);
    static Variant int16(int16_t v);
    static Variant int32(int32_t v);
    static Variant int64(int64_t v);
    static Variant uint8(uint8_t v);
    static Variant uint16(uint16_t v);
    static Variant uint32(uint32_t v);
    static Variant uint64(uint64_t v);
    static Variant float32(float v);
    static Variant float64(double v);
    static Variant time(int64_t microseconds);
    static Variant version(uint64_t v);
    static Variant string(std::string_view s);
    static Variant binary(std::span<const std::byte> b);

    // === Accessors ===
    [[nodiscard]] DataType type() const;
    [[nodiscard]] bool is_none() const;

    // Type-safe value extraction. Aborts on type mismatch.
    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] int64_t as_int() const;   // Any signed integer → int64_t
    [[nodiscard]] uint64_t as_uint() const; // Any unsigned integer → uint64_t
    [[nodiscard]] double as_float() const;  // Any floating point → double
    [[nodiscard]] std::string_view as_string() const;
    [[nodiscard]] std::span<const std::byte> as_binary() const;

    // === Comparison ===
    std::strong_ordering operator<=>(const Variant& other) const;
    bool operator==(const Variant& other) const;

    // === Serialization ===
    // Fixed-size types: little-endian fixed bytes.
    // Variable-size types: varint length prefix + data.
    void encode_to(std::string& out) const;
    static absl::StatusOr<Variant> decode_from(DataType type, std::span<const std::byte> data);

private:
    Variant() : type_(DataType::kNone), int_val_(0) {}

    void destroy();
    void copy_from(const Variant& other);
    void move_from(Variant&& other) noexcept;

    // Indicates whether the active union member is str_val_
    [[nodiscard]] bool uses_string_storage() const;

    DataType type_;

    union {
        bool bool_val_;
        int64_t int_val_;
        uint64_t uint_val_;
        double double_val_;
        std::string str_val_; // Backing store for string and binary
    };
};

} // namespace pl::sstv2::types
