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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::types {

// =============================================================================
// Compound scalar types for column-store decomposition.
// =============================================================================

struct LongDouble {
    uint8_t data[16]{};

    bool operator==(const LongDouble& rhs) const { return std::memcmp(data, rhs.data, 16) == 0; }
    bool operator!=(const LongDouble& rhs) const { return !(*this == rhs); }
};

struct Time {
    int64_t seconds = 0;
    uint32_t nanoseconds = 0;

    bool operator==(const Time& rhs) const {
        return seconds == rhs.seconds && nanoseconds == rhs.nanoseconds;
    }
    bool operator!=(const Time& rhs) const { return !(*this == rhs); }
};

struct Version {
    uint64_t major = 0;
    uint64_t minor = 0;

    bool operator==(const Version& rhs) const { return major == rhs.major && minor == rhs.minor; }
    bool operator!=(const Version& rhs) const { return !(*this == rhs); }
};

// =============================================================================
// StorageCategory: how a DataType is physically stored inside Value.
//
// This reduces lifecycle management from 20+ types to 4 categories.
// =============================================================================

enum class StorageCategory : uint8_t {
    kNone,   // No storage (DataType::kNone)
    kInline, // Fixed-size scalars stored inline (bool..LongDouble, Time, Version)
    kString, // Variable-length bytes (String, Binary, U16String, U32String)
    kArray,  // std::vector<Value>
    kMap,    // std::vector<std::pair<Value, Value>>
};

// =============================================================================
// TypeTraits<DT>: compile-time properties for each DataType.
//
// To add a new type, specialize this template. Everything else adapts
// automatically through the traits.
// =============================================================================

template <DataType DT> struct TypeTraits;

// clang-format off

template <> struct TypeTraits<DataType::kNone> {
    using native_type = void;
    static constexpr StorageCategory category = StorageCategory::kNone;
    static constexpr size_t inline_size = 0;
};

template <> struct TypeTraits<DataType::kBool> {
    using native_type = bool;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(bool);
};

template <> struct TypeTraits<DataType::kInt8> {
    using native_type = int8_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(int8_t);
};

template <> struct TypeTraits<DataType::kUint8> {
    using native_type = uint8_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(uint8_t);
};

template <> struct TypeTraits<DataType::kInt16> {
    using native_type = int16_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(int16_t);
};

template <> struct TypeTraits<DataType::kUint16> {
    using native_type = uint16_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(uint16_t);
};

template <> struct TypeTraits<DataType::kInt32> {
    using native_type = int32_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(int32_t);
};

template <> struct TypeTraits<DataType::kUint32> {
    using native_type = uint32_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(uint32_t);
};

template <> struct TypeTraits<DataType::kInt64> {
    using native_type = int64_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(int64_t);
};

template <> struct TypeTraits<DataType::kUint64> {
    using native_type = uint64_t;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(uint64_t);
};

template <> struct TypeTraits<DataType::kFloat> {
    using native_type = float;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(float);
};

template <> struct TypeTraits<DataType::kDouble> {
    using native_type = double;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(double);
};

template <> struct TypeTraits<DataType::kLongDouble> {
    using native_type = LongDouble;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(LongDouble);
};

template <> struct TypeTraits<DataType::kTime> {
    using native_type = Time;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(Time);
};

template <> struct TypeTraits<DataType::kVersion> {
    using native_type = Version;
    static constexpr StorageCategory category = StorageCategory::kInline;
    static constexpr size_t inline_size = sizeof(Version);
};

template <> struct TypeTraits<DataType::kString> {
    using native_type = std::string;
    static constexpr StorageCategory category = StorageCategory::kString;
    static constexpr size_t inline_size = 0;
};

template <> struct TypeTraits<DataType::kU16String> {
    using native_type = std::string;
    static constexpr StorageCategory category = StorageCategory::kString;
    static constexpr size_t inline_size = 0;
};

template <> struct TypeTraits<DataType::kU32String> {
    using native_type = std::string;
    static constexpr StorageCategory category = StorageCategory::kString;
    static constexpr size_t inline_size = 0;
};

template <> struct TypeTraits<DataType::kBinary> {
    using native_type = std::string;
    static constexpr StorageCategory category = StorageCategory::kString;
    static constexpr size_t inline_size = 0;
};

// clang-format on

// Forward declaration for Array/Map native types.
class Value;

using ArrayStorage = std::vector<Value>;
using MapStorage = std::vector<std::pair<Value, Value>>;

template <> struct TypeTraits<DataType::kArray> {
    using native_type = ArrayStorage;
    static constexpr StorageCategory category = StorageCategory::kArray;
    static constexpr size_t inline_size = 0;
};

template <> struct TypeTraits<DataType::kMap> {
    using native_type = MapStorage;
    static constexpr StorageCategory category = StorageCategory::kMap;
    static constexpr size_t inline_size = 0;
};

// Convenience alias.
template <DataType DT> using native_type_t = typename TypeTraits<DT>::native_type;

// =============================================================================
// Runtime StorageCategory lookup.
// =============================================================================

constexpr StorageCategory storage_category_of(DataType dt) {
    switch (dt) {
        case DataType::kNone:
        case DataType::kDataBlock:
        case DataType::kIndexBlock:
            return StorageCategory::kNone;
        case DataType::kBool:
        case DataType::kInt8:
        case DataType::kUint8:
        case DataType::kInt16:
        case DataType::kUint16:
        case DataType::kInt32:
        case DataType::kUint32:
        case DataType::kInt64:
        case DataType::kUint64:
        case DataType::kFloat:
        case DataType::kDouble:
        case DataType::kLongDouble:
        case DataType::kTime:
        case DataType::kVersion:
            return StorageCategory::kInline;
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary:
            return StorageCategory::kString;
        case DataType::kArray:
            return StorageCategory::kArray;
        case DataType::kMap:
            return StorageCategory::kMap;
    }
    return StorageCategory::kNone;
}

// =============================================================================
// Value: a type-safe tagged union for SSTableV2 column data.
//
// Internally uses a discriminated union with manual lifecycle management.
// Storage is categorized into 4 kinds (None/Inline/String/Array/Map),
// reducing lifecycle code to O(categories) rather than O(types).
//
// Usage:
//   Value v = Value::make<DataType::kInt64>(42);
//   int64_t x = v.get<DataType::kInt64>();
//
//   Value s = Value::make<DataType::kString>("hello");
//   std::string_view sv = s.as_string();
//
//   Value arr = Value::make_array({Value::make<DataType::kInt32>(1),
//                                  Value::make<DataType::kInt32>(2)});
// =============================================================================

class Value {
public:
    // --- Construction / Destruction ---

    Value() noexcept : type_(DataType::kNone) {}

    ~Value() { destroy(); }

    Value(const Value& other) : type_(DataType::kNone) { copy_from(other); }

    Value(Value&& other) noexcept : type_(DataType::kNone) { move_from(std::move(other)); }

    Value& operator=(const Value& other) {
        if (this != &other) {
            destroy();
            copy_from(other);
        }
        return *this;
    }

    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    // --- Static factory: compile-time typed construction ---

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kInline, int> = 0>
    static Value make(native_type_t<DT> v) {
        Value val;
        val.type_ = DT;
        new (&val.storage_.inline_data) native_type_t<DT>(std::move(v));
        return val;
    }

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kString, int> = 0>
    static Value make(std::string v) {
        Value val;
        val.type_ = DT;
        new (&val.storage_.str) std::string(std::move(v));
        return val;
    }

    // String-like from string_view (copies).
    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kString, int> = 0>
    static Value make(std::string_view sv) {
        return make<DT>(std::string(sv));
    }

    // String-like from const char* (copies).
    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kString, int> = 0>
    static Value make(const char* s) {
        return make<DT>(std::string(s));
    }

    // Array construction.
    static Value make_array(std::vector<Value> elements) {
        Value val;
        val.type_ = DataType::kArray;
        new (&val.storage_.array) ArrayStorage(std::move(elements));
        return val;
    }

    // Map construction. Entries are canonicalized by key so Map ordering is
    // independent from insertion order.
    static Value make_map(std::vector<std::pair<Value, Value>> entries);

    // --- Type query ---

    [[nodiscard]] DataType type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == DataType::kNone; }
    [[nodiscard]] StorageCategory category() const noexcept { return storage_category_of(type_); }

    // --- Equality ---

    bool operator==(const Value& rhs) const {
        if (type_ != rhs.type_)
            return false;
        switch (storage_category_of(type_)) {
            case StorageCategory::kNone:
                return true;
            case StorageCategory::kInline:
                return std::memcmp(
                           &storage_.inline_data, &rhs.storage_.inline_data, kInlineCapacity) == 0;
            case StorageCategory::kString:
                return storage_.str == rhs.storage_.str;
            case StorageCategory::kArray:
                return storage_.array == rhs.storage_.array;
            case StorageCategory::kMap:
                return storage_.map == rhs.storage_.map;
        }
        return false;
    }

    bool operator!=(const Value& rhs) const { return !(*this == rhs); }

    // --- Typed access (compile-time checked) ---

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kInline, int> = 0>
    [[nodiscard]] const native_type_t<DT>& ref() const {
        assert(type_ == DT);
        return *reinterpret_cast<const native_type_t<DT>*>(&storage_.inline_data);
    }

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kInline, int> = 0>
    [[nodiscard]] native_type_t<DT> get() const {
        return ref<DT>();
    }

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kString, int> = 0>
    [[nodiscard]] const std::string& ref() const {
        assert(storage_category_of(type_) == StorageCategory::kString);
        return storage_.str;
    }

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kString, int> = 0>
    [[nodiscard]] std::string get() const {
        return ref<DT>();
    }

    // Move out the stored value.
    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kInline, int> = 0>
    native_type_t<DT> take() {
        assert(type_ == DT);
        auto v = std::move(*reinterpret_cast<native_type_t<DT>*>(&storage_.inline_data));
        type_ = DataType::kNone;
        return v;
    }

    template <DataType DT,
              std::enable_if_t<TypeTraits<DT>::category == StorageCategory::kString, int> = 0>
    std::string take() {
        assert(storage_category_of(type_) == StorageCategory::kString);
        auto v = std::move(storage_.str);
        storage_.str.~basic_string();
        type_ = DataType::kNone;
        return v;
    }

    // --- Convenience accessors ---

    [[nodiscard]] bool as_bool() const { return get<DataType::kBool>(); }
    [[nodiscard]] int8_t as_int8() const { return get<DataType::kInt8>(); }
    [[nodiscard]] uint8_t as_uint8() const { return get<DataType::kUint8>(); }
    [[nodiscard]] int16_t as_int16() const { return get<DataType::kInt16>(); }
    [[nodiscard]] uint16_t as_uint16() const { return get<DataType::kUint16>(); }
    [[nodiscard]] int32_t as_int32() const { return get<DataType::kInt32>(); }
    [[nodiscard]] uint32_t as_uint32() const { return get<DataType::kUint32>(); }
    [[nodiscard]] int64_t as_int64() const { return get<DataType::kInt64>(); }
    [[nodiscard]] uint64_t as_uint64() const { return get<DataType::kUint64>(); }
    [[nodiscard]] float as_float() const { return get<DataType::kFloat>(); }
    [[nodiscard]] double as_double() const { return get<DataType::kDouble>(); }
    [[nodiscard]] const LongDouble& as_long_double() const { return ref<DataType::kLongDouble>(); }
    [[nodiscard]] const Time& as_time() const { return ref<DataType::kTime>(); }
    [[nodiscard]] const Version& as_version() const { return ref<DataType::kVersion>(); }

    [[nodiscard]] std::string_view as_string() const {
        assert(storage_category_of(type_) == StorageCategory::kString);
        return storage_.str;
    }

    [[nodiscard]] std::string_view as_binary() const {
        assert(type_ == DataType::kBinary);
        return storage_.str;
    }

    [[nodiscard]] const ArrayStorage& as_array() const {
        assert(type_ == DataType::kArray);
        return storage_.array;
    }

    [[nodiscard]] const MapStorage& as_map() const {
        assert(type_ == DataType::kMap);
        return storage_.map;
    }

    // Move out string.
    std::string take_string() { return take<DataType::kString>(); }

    // Move out array.
    ArrayStorage take_array() {
        assert(type_ == DataType::kArray);
        auto v = std::move(storage_.array);
        storage_.array.~vector();
        type_ = DataType::kNone;
        return v;
    }

    // Move out map.
    MapStorage take_map() {
        assert(type_ == DataType::kMap);
        auto v = std::move(storage_.map);
        storage_.map.~vector();
        type_ = DataType::kNone;
        return v;
    }

    // --- Visitor support ---

    // Dispatches to visitor based on runtime type. The visitor is called with
    // (const native_type_t<DT>&) for the active type, or with std::monostate
    // for kNone.
    template <typename Visitor> decltype(auto) visit(Visitor&& vis) const {
        return dispatch_visit(std::forward<Visitor>(vis));
    }

    template <typename Visitor> decltype(auto) visit(Visitor&& vis) {
        return dispatch_visit_mut(std::forward<Visitor>(vis));
    }

private:
    // --- Storage ---

    // Inline capacity: large enough for the biggest inline type.
    static constexpr size_t kInlineCapacity =
        24; // >= sizeof(LongDouble), sizeof(Time), sizeof(Version)
    static_assert(sizeof(LongDouble) <= kInlineCapacity);
    static_assert(sizeof(Time) <= kInlineCapacity);
    static_assert(sizeof(Version) <= kInlineCapacity);

    union Storage {
        Storage() noexcept : inline_data{} {}
        ~Storage() {} // Destruction managed by Value.

        std::aligned_storage_t<kInlineCapacity, alignof(uint64_t)> inline_data;
        std::string str;
        ArrayStorage array;
        MapStorage map;
    };

    DataType type_;
    Storage storage_;

    // --- Lifecycle helpers ---

    void destroy() {
        switch (storage_category_of(type_)) {
            case StorageCategory::kNone:
            case StorageCategory::kInline:
                break; // Trivially destructible or POD-like.
            case StorageCategory::kString:
                storage_.str.~basic_string();
                break;
            case StorageCategory::kArray:
                storage_.array.~vector();
                break;
            case StorageCategory::kMap:
                storage_.map.~vector();
                break;
        }
        type_ = DataType::kNone;
    }

    void copy_from(const Value& other) {
        switch (storage_category_of(other.type_)) {
            case StorageCategory::kNone:
                break;
            case StorageCategory::kInline:
                std::memcpy(&storage_.inline_data, &other.storage_.inline_data, kInlineCapacity);
                break;
            case StorageCategory::kString:
                new (&storage_.str) std::string(other.storage_.str);
                break;
            case StorageCategory::kArray:
                new (&storage_.array) ArrayStorage(other.storage_.array);
                break;
            case StorageCategory::kMap:
                new (&storage_.map) MapStorage(other.storage_.map);
                break;
        }
        type_ = other.type_; // Only set AFTER successful construction
    }

    void move_from(Value&& other) noexcept {
        type_ = other.type_;
        switch (storage_category_of(other.type_)) {
            case StorageCategory::kNone:
                break;
            case StorageCategory::kInline:
                std::memcpy(&storage_.inline_data, &other.storage_.inline_data, kInlineCapacity);
                break;
            case StorageCategory::kString:
                new (&storage_.str) std::string(std::move(other.storage_.str));
                other.storage_.str.~basic_string();
                break;
            case StorageCategory::kArray:
                new (&storage_.array) ArrayStorage(std::move(other.storage_.array));
                other.storage_.array.~vector();
                break;
            case StorageCategory::kMap:
                new (&storage_.map) MapStorage(std::move(other.storage_.map));
                other.storage_.map.~vector();
                break;
        }
        other.type_ = DataType::kNone;
    }

    // --- Visitor dispatch ---

    struct Monostate {};

    template <typename Visitor> decltype(auto) dispatch_visit(Visitor&& vis) const {
        switch (type_) {
            case DataType::kBool:
                return vis(ref<DataType::kBool>());
            case DataType::kInt8:
                return vis(ref<DataType::kInt8>());
            case DataType::kUint8:
                return vis(ref<DataType::kUint8>());
            case DataType::kInt16:
                return vis(ref<DataType::kInt16>());
            case DataType::kUint16:
                return vis(ref<DataType::kUint16>());
            case DataType::kInt32:
                return vis(ref<DataType::kInt32>());
            case DataType::kUint32:
                return vis(ref<DataType::kUint32>());
            case DataType::kInt64:
                return vis(ref<DataType::kInt64>());
            case DataType::kUint64:
                return vis(ref<DataType::kUint64>());
            case DataType::kFloat:
                return vis(ref<DataType::kFloat>());
            case DataType::kDouble:
                return vis(ref<DataType::kDouble>());
            case DataType::kLongDouble:
                return vis(ref<DataType::kLongDouble>());
            case DataType::kTime:
                return vis(ref<DataType::kTime>());
            case DataType::kVersion:
                return vis(ref<DataType::kVersion>());
            case DataType::kString:
            case DataType::kU16String:
            case DataType::kU32String:
            case DataType::kBinary:
                return vis(storage_.str);
            case DataType::kArray:
                return vis(storage_.array);
            case DataType::kMap:
                return vis(storage_.map);
            default:
                return vis(Monostate{});
        }
    }

    template <typename Visitor> decltype(auto) dispatch_visit_mut(Visitor&& vis) {
        switch (type_) {
            case DataType::kBool:
                return vis(*reinterpret_cast<bool*>(&storage_.inline_data));
            case DataType::kInt8:
                return vis(*reinterpret_cast<int8_t*>(&storage_.inline_data));
            case DataType::kUint8:
                return vis(*reinterpret_cast<uint8_t*>(&storage_.inline_data));
            case DataType::kInt16:
                return vis(*reinterpret_cast<int16_t*>(&storage_.inline_data));
            case DataType::kUint16:
                return vis(*reinterpret_cast<uint16_t*>(&storage_.inline_data));
            case DataType::kInt32:
                return vis(*reinterpret_cast<int32_t*>(&storage_.inline_data));
            case DataType::kUint32:
                return vis(*reinterpret_cast<uint32_t*>(&storage_.inline_data));
            case DataType::kInt64:
                return vis(*reinterpret_cast<int64_t*>(&storage_.inline_data));
            case DataType::kUint64:
                return vis(*reinterpret_cast<uint64_t*>(&storage_.inline_data));
            case DataType::kFloat:
                return vis(*reinterpret_cast<float*>(&storage_.inline_data));
            case DataType::kDouble:
                return vis(*reinterpret_cast<double*>(&storage_.inline_data));
            case DataType::kLongDouble:
                return vis(*reinterpret_cast<LongDouble*>(&storage_.inline_data));
            case DataType::kTime:
                return vis(*reinterpret_cast<Time*>(&storage_.inline_data));
            case DataType::kVersion:
                return vis(*reinterpret_cast<Version*>(&storage_.inline_data));
            case DataType::kString:
            case DataType::kU16String:
            case DataType::kU32String:
            case DataType::kBinary:
                return vis(storage_.str);
            case DataType::kArray:
                return vis(storage_.array);
            case DataType::kMap:
                return vis(storage_.map);
            default:
                return vis(Monostate{});
        }
    }
};

inline int compare_values(const Value& lhs, const Value& rhs);

inline int compare_bytes(std::string_view lhs, std::string_view rhs) {
    const int cmp = lhs.compare(rhs);
    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    return 0;
}

template <typename T> inline int compare_scalar(const T& lhs, const T& rhs) {
    if (lhs < rhs)
        return -1;
    if (rhs < lhs)
        return 1;
    return 0;
}

template <typename T> inline int compare_floating(T lhs, T rhs) {
    const bool lhs_nan = std::isnan(lhs);
    const bool rhs_nan = std::isnan(rhs);
    if (lhs_nan || rhs_nan) {
        if (lhs_nan == rhs_nan)
            return 0;
        return lhs_nan ? -1 : 1;
    }
    return compare_scalar(lhs, rhs);
}

inline int compare_arrays(const ArrayStorage& lhs, const ArrayStorage& rhs) {
    const size_t n = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < n; ++i) {
        const int cmp = compare_values(lhs[i], rhs[i]);
        if (cmp != 0)
            return cmp;
    }
    return compare_scalar(lhs.size(), rhs.size());
}

inline int compare_maps(const MapStorage& lhs, const MapStorage& rhs) {
    const size_t n = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < n; ++i) {
        int cmp = compare_values(lhs[i].first, rhs[i].first);
        if (cmp != 0)
            return cmp;
        cmp = compare_values(lhs[i].second, rhs[i].second);
        if (cmp != 0)
            return cmp;
    }
    return compare_scalar(lhs.size(), rhs.size());
}

inline int compare_values(const Value& lhs, const Value& rhs) {
    if (lhs.type() != rhs.type()) {
        return compare_scalar(static_cast<uint8_t>(lhs.type()), static_cast<uint8_t>(rhs.type()));
    }

    switch (lhs.type()) {
        case DataType::kNone:
            return 0;
        case DataType::kBool:
            return compare_scalar(lhs.as_bool(), rhs.as_bool());
        case DataType::kInt8:
            return compare_scalar(lhs.as_int8(), rhs.as_int8());
        case DataType::kUint8:
            return compare_scalar(lhs.as_uint8(), rhs.as_uint8());
        case DataType::kInt16:
            return compare_scalar(lhs.as_int16(), rhs.as_int16());
        case DataType::kUint16:
            return compare_scalar(lhs.as_uint16(), rhs.as_uint16());
        case DataType::kInt32:
            return compare_scalar(lhs.as_int32(), rhs.as_int32());
        case DataType::kUint32:
            return compare_scalar(lhs.as_uint32(), rhs.as_uint32());
        case DataType::kInt64:
            return compare_scalar(lhs.as_int64(), rhs.as_int64());
        case DataType::kUint64:
            return compare_scalar(lhs.as_uint64(), rhs.as_uint64());
        case DataType::kFloat:
            return compare_floating(lhs.as_float(), rhs.as_float());
        case DataType::kDouble:
            return compare_floating(lhs.as_double(), rhs.as_double());
        case DataType::kLongDouble:
            return compare_bytes(
                std::string_view(reinterpret_cast<const char*>(lhs.as_long_double().data),
                                 sizeof(lhs.as_long_double().data)),
                std::string_view(reinterpret_cast<const char*>(rhs.as_long_double().data),
                                 sizeof(rhs.as_long_double().data)));
        case DataType::kTime: {
            int cmp = compare_scalar(lhs.as_time().seconds, rhs.as_time().seconds);
            return cmp != 0 ? cmp
                            : compare_scalar(lhs.as_time().nanoseconds, rhs.as_time().nanoseconds);
        }
        case DataType::kVersion: {
            int cmp = compare_scalar(lhs.as_version().major, rhs.as_version().major);
            return cmp != 0 ? cmp : compare_scalar(lhs.as_version().minor, rhs.as_version().minor);
        }
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary:
            return compare_bytes(lhs.as_string(), rhs.as_string());
        case DataType::kArray:
            return compare_arrays(lhs.as_array(), rhs.as_array());
        case DataType::kMap:
            return compare_maps(lhs.as_map(), rhs.as_map());
        default:
            return 0;
    }
}

inline Value Value::make_map(std::vector<std::pair<Value, Value>> entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return compare_values(lhs.first, rhs.first) < 0;
    });
    Value val;
    val.type_ = DataType::kMap;
    new (&val.storage_.map) MapStorage(std::move(entries));
    return val;
}

// =============================================================================
// NativeTypeMapping: reverse mapping from C++ type to DataType.
// Used by make_value() for automatic type deduction.
// =============================================================================

namespace detail {

template <typename T> struct NativeTypeMapping;

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
template <> struct NativeTypeMapping<LongDouble>  { static constexpr DataType value = DataType::kLongDouble; };
template <> struct NativeTypeMapping<Time>        { static constexpr DataType value = DataType::kTime; };
template <> struct NativeTypeMapping<Version>     { static constexpr DataType value = DataType::kVersion; };
// clang-format on

} // namespace detail

// make_value: construct a Value from a native scalar, inferring DataType.
template <typename T,
          std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string> &&
                               !std::is_same_v<std::decay_t<T>, const char*> &&
                               !std::is_same_v<std::decay_t<T>, std::string_view>,
                           int> = 0>
Value make_value(T v) {
    constexpr DataType dt = detail::NativeTypeMapping<std::decay_t<T>>::value;
    return Value::make<dt>(std::move(v));
}

} // namespace pl::sstv2::types
