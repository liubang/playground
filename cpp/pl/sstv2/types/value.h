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

#include "cpp/pl/sstv2/types/data_type.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace pl::sstv2::types {

// =============================================================================
// TypeMapping: compile-time mapping from DataType enum to C++ native type.
// =============================================================================

template <DataType DT>
struct TypeMapping;

// clang-format off
template <> struct TypeMapping<DataType::kBool>       { using type = bool; };
template <> struct TypeMapping<DataType::kInt8>       { using type = int8_t; };
template <> struct TypeMapping<DataType::kUint8>      { using type = uint8_t; };
template <> struct TypeMapping<DataType::kInt16>      { using type = int16_t; };
template <> struct TypeMapping<DataType::kUint16>     { using type = uint16_t; };
template <> struct TypeMapping<DataType::kInt32>      { using type = int32_t; };
template <> struct TypeMapping<DataType::kUint32>     { using type = uint32_t; };
template <> struct TypeMapping<DataType::kInt64>      { using type = int64_t; };
template <> struct TypeMapping<DataType::kUint64>     { using type = uint64_t; };
template <> struct TypeMapping<DataType::kFloat>      { using type = float; };
template <> struct TypeMapping<DataType::kDouble>     { using type = double; };
template <> struct TypeMapping<DataType::kString>     { using type = std::string; };
template <> struct TypeMapping<DataType::kBinary>     { using type = std::string; };
// clang-format on

template <DataType DT>
using mapped_type_t = typename TypeMapping<DT>::type;

// =============================================================================
// Value: a type-safe tagged variant for SSTableV2 column data.
//
// Usage:
//   Value v = Value::make<DataType::kInt64>(42);
//   int64_t x = v.get<DataType::kInt64>();
//
//   Value s = Value::make<DataType::kString>("hello");
//   std::string_view sv = s.ref<DataType::kString>();
//
// The variant is discriminated at runtime by DataType, but accessors are
// parameterized at compile time for type safety. Accessing with the wrong
// DataType tag is undefined behavior (debug builds may assert).
// =============================================================================

class Value {
public:
    // Monostate represents DataType::kNone.
    Value() : type_(DataType::kNone), storage_(std::monostate{}) {}

    // Compile-time typed construction.
    template <DataType DT>
    static Value make(mapped_type_t<DT> v) {
        Value val;
        val.type_ = DT;
        val.storage_.template emplace<mapped_type_t<DT>>(std::move(v));
        return val;
    }

    // For String/Binary: construct from string_view (copies).
    template <DataType DT, typename = std::enable_if_t<
                               DT == DataType::kString || DT == DataType::kBinary>>
    static Value make(std::string_view sv) {
        return make<DT>(std::string(sv));
    }

    // For String/Binary: construct from const char* (copies).
    template <DataType DT, typename = std::enable_if_t<
                               DT == DataType::kString || DT == DataType::kBinary>,
              typename = void>
    static Value make(const char* s) {
        return make<DT>(std::string(s));
    }

    // Runtime type tag.
    DataType type() const { return type_; }
    bool is_null() const { return type_ == DataType::kNone; }

    // Typed access (by value, for scalars).
    template <DataType DT>
    mapped_type_t<DT> get() const {
        return std::get<mapped_type_t<DT>>(storage_);
    }

    // Typed access (by const reference, for heap types).
    template <DataType DT>
    const mapped_type_t<DT>& ref() const {
        return std::get<mapped_type_t<DT>>(storage_);
    }

    // Move out the stored value (for heap types).
    template <DataType DT>
    mapped_type_t<DT> take() {
        type_ = DataType::kNone;
        return std::move(std::get<mapped_type_t<DT>>(storage_));
    }

    // =========================================================================
    // Convenience accessors (non-template, for runtime-dispatched code paths).
    // =========================================================================

    bool as_bool() const { return get<DataType::kBool>(); }
    int8_t as_int8() const { return get<DataType::kInt8>(); }
    uint8_t as_uint8() const { return get<DataType::kUint8>(); }
    int16_t as_int16() const { return get<DataType::kInt16>(); }
    uint16_t as_uint16() const { return get<DataType::kUint16>(); }
    int32_t as_int32() const { return get<DataType::kInt32>(); }
    uint32_t as_uint32() const { return get<DataType::kUint32>(); }
    int64_t as_int64() const { return get<DataType::kInt64>(); }
    uint64_t as_uint64() const { return get<DataType::kUint64>(); }
    float as_float() const { return get<DataType::kFloat>(); }
    double as_double() const { return get<DataType::kDouble>(); }
    std::string_view as_string() const { return ref<DataType::kString>(); }
    std::string_view as_binary() const { return ref<DataType::kBinary>(); }

    // Move out the underlying string.
    std::string take_string() { return take<DataType::kString>(); }

    // =========================================================================
    // Visitor support: apply a callable to the stored value.
    // =========================================================================

    template <typename Visitor>
    decltype(auto) visit(Visitor&& vis) const {
        return std::visit(std::forward<Visitor>(vis), storage_);
    }

    template <typename Visitor>
    decltype(auto) visit(Visitor&& vis) {
        return std::visit(std::forward<Visitor>(vis), storage_);
    }

private:
    DataType type_;

    // std::monostate for kNone; String and Binary share std::string.
    // Note: kString and kBinary map to the same C++ type (std::string),
    // so they occupy the same variant alternative. The DataType tag
    // disambiguates them at runtime.
    using Storage = std::variant<
        std::monostate, // kNone
        bool,           // kBool
        int8_t,         // kInt8
        uint8_t,        // kUint8
        int16_t,        // kInt16
        uint16_t,       // kUint16
        int32_t,        // kInt32
        uint32_t,       // kUint32
        int64_t,        // kInt64
        uint64_t,       // kUint64
        float,          // kFloat
        double,         // kDouble
        std::string     // kString, kBinary
    >;

    Storage storage_;
};

// =============================================================================
// Deduction helper: create Value from a native C++ value with automatic
// DataType inference.
// =============================================================================

namespace detail {

template <typename T>
struct NativeTypeMapping;

// clang-format off
template <> struct NativeTypeMapping<bool>        { static constexpr DataType value = DataType::kBool; };
template <> struct NativeTypeMapping<int8_t>      { static constexpr DataType value = DataType::kInt8; };
template <> struct NativeTypeMapping<uint8_t>     { static constexpr DataType value = DataType::kUint8; };
template <> struct NativeTypeMapping<int16_t>     { static constexpr DataType value = DataType::kInt16; };
template <> struct NativeTypeMapping<uint16_t>    { static constexpr DataType value = DataType::kUint16; };
template <> struct NativeTypeMapping<int32_t>     { static constexpr DataType value = DataType::kInt32; };
template <> struct NativeTypeMapping<uint32_t>    { static constexpr DataType value = DataType::kUint32; };
template <> struct NativeTypeMapping<int64_t>     { static constexpr DataType value = DataType::kInt64; };
template <> struct NativeTypeMapping<uint64_t>    { static constexpr DataType value = DataType::kUint64; };
template <> struct NativeTypeMapping<float>       { static constexpr DataType value = DataType::kFloat; };
template <> struct NativeTypeMapping<double>      { static constexpr DataType value = DataType::kDouble; };
// clang-format on

} // namespace detail

// make_value: construct a Value from a native scalar, inferring DataType.
template <typename T, typename = std::enable_if_t<
                          !std::is_same_v<std::decay_t<T>, std::string> &&
                          !std::is_same_v<std::decay_t<T>, const char*> &&
                          !std::is_same_v<std::decay_t<T>, std::string_view>>>
Value make_value(T v) {
    constexpr DataType dt = detail::NativeTypeMapping<std::decay_t<T>>::value;
    return Value::make<dt>(v);
}

} // namespace pl::sstv2::types
