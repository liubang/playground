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
// Created: 2026/06/05 21:13

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "cpp/pl/sstv2/codec/varint.h"
#include "cpp/pl/sstv2/pattern/raw.h"
#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::pattern {

using types::DataType;

// =============================================================================
// detail: shared compound wire-format helpers.
// =============================================================================

namespace detail {

inline void write_compound_header(size_t sub_count, std::string* dst) {
    dst->push_back(static_cast<char>(static_cast<uint8_t>(PatternId::kCompound)));
    codec::encode_varint(sub_count, dst);
}

template <size_t N>
inline void write_compound_units(const std::array<std::pair<DataType, std::string_view>, N>& units,
                                 std::string* dst) {
    write_compound_header(N, dst);
    uint64_t offset = 0;
    for (const auto& [type, unit] : units) {
        dst->push_back(static_cast<char>(static_cast<uint8_t>(type)));
        codec::encode_varint(offset, dst);
        offset += unit.size();
    }
    for (const auto& [_, unit] : units) {
        dst->append(unit.data(), unit.size());
    }
}

// Parse compound header. Returns false on mismatch. Advances pos past header.
[[nodiscard]] inline bool parse_compound_header(const uint8_t* src,
                                                size_t len,
                                                size_t expected_sub_count,
                                                size_t* pos) noexcept {
    if (len < 1 || src[0] != static_cast<uint8_t>(PatternId::kCompound)) {
        return false;
    }
    *pos = 1;

    uint64_t sub_count = 0;
    size_t n = codec::decode_varint(src + *pos, len - *pos, &sub_count);
    if (n == 0 || sub_count != expected_sub_count) {
        return false;
    }
    *pos += n;
    return true;
}

template <size_t N>
[[nodiscard]] inline bool parse_compound_units(std::string_view input,
                                               const std::array<DataType, N>& expected_types,
                                               std::array<std::string_view, N>* units,
                                               size_t* bytes_consumed) noexcept {
    const auto* src = reinterpret_cast<const uint8_t*>(input.data());
    const size_t len = input.size();
    size_t pos = 0;
    if (!parse_compound_header(src, len, N, &pos)) {
        return false;
    }

    std::array<uint64_t, N> offsets{};
    for (size_t i = 0; i < N; ++i) {
        if (pos >= len || src[pos] != static_cast<uint8_t>(expected_types[i])) {
            return false;
        }
        ++pos;
        uint64_t offset = 0;
        const size_t n = codec::decode_varint(src + pos, len - pos, &offset);
        if (n == 0) {
            return false;
        }
        pos += n;
        if (i > 0 && offset < offsets[i - 1]) {
            return false;
        }
        offsets[i] = offset;
    }

    const size_t data_start = pos;
    for (size_t i = 0; i < N; ++i) {
        const uint64_t begin = offsets[i];
        const uint64_t end = i + 1 == N ? len - data_start : offsets[i + 1];
        if (begin > end || end > len - data_start) {
            return false;
        }
        (*units)[i] =
            input.substr(data_start + static_cast<size_t>(begin), static_cast<size_t>(end - begin));
    }
    *bytes_consumed = len;
    return true;
}

} // namespace detail

// =============================================================================
// CompoundEncoder<DT>: primary template (intentionally undefined).
// Only valid DataType specializations can be instantiated.
// =============================================================================

template <DataType DT> class CompoundEncoder;

template <DataType DT> class CompoundDecoder;

// =============================================================================
// CompoundEncoder<DataType::kString>
//
// Encodes string-like types as (offset: u64, length: u64) pairs referencing
// positions in the Data Table.
// Also used for kBinary, kU16String, kU32String.
// =============================================================================

template <> class CompoundEncoder<DataType::kString> {
public:
    CompoundEncoder() = default;

    void reset() noexcept {
        offsets_.reset();
        lengths_.reset();
        row_count_ = 0;
    }

    void reserve(size_t expected_rows) {
        offsets_.reserve(expected_rows);
        lengths_.reserve(expected_rows);
    }

    void add(uint64_t offset, uint64_t length) noexcept {
        offsets_.add(offset);
        lengths_.add(length);
        ++row_count_;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }

    void finish_into(std::string* dst) const {
        const std::string offsets = offsets_.finish().data;
        const std::string lengths = lengths_.finish().data;
        detail::write_compound_units<2>(
            {{{DataType::kUint64, offsets}, {DataType::kUint64, lengths}}}, dst);
    }

    [[nodiscard]] EncodeResult finish() const {
        EncodeResult r;
        r.row_count = row_count_;
        finish_into(&r.data);
        return r;
    }

private:
    RawEncoder<8> offsets_;
    RawEncoder<8> lengths_;
    size_t row_count_ = 0;
};

// =============================================================================
// CompoundDecoder<DataType::kString>
// =============================================================================

template <> class CompoundDecoder<DataType::kString> {
public:
    CompoundDecoder() = default;

    [[nodiscard]] bool parse(std::string_view input) noexcept {
        std::array<std::string_view, 2> units;
        if (!detail::parse_compound_units<2>(
                input, {DataType::kUint64, DataType::kUint64}, &units, &bytes_consumed_))
            return false;
        if (!offsets_.parse(units[0])) {
            return false;
        }
        if (!lengths_.parse(units[1])) {
            return false;
        }
        if (offsets_.bytes_consumed() != units[0].size()) {
            return false;
        }
        bytes_consumed_ = bytes_consumed_ - units[1].size() + lengths_.bytes_consumed();

        if (offsets_.row_count() != lengths_.row_count()) {
            return false;
        }

        row_count_ = offsets_.row_count();
        return true;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }
    [[nodiscard]] size_t bytes_consumed() const noexcept { return bytes_consumed_; }

    [[nodiscard]] uint64_t offset(size_t i) const noexcept { return offsets_.get(i); }
    [[nodiscard]] uint64_t length(size_t i) const noexcept { return lengths_.get(i); }

private:
    RawDecoder<8> offsets_;
    RawDecoder<8> lengths_;
    size_t row_count_ = 0;
    size_t bytes_consumed_ = 0;
};

// =============================================================================
// CompoundEncoder<DataType::kTime>
//
// Encodes Time as (seconds: i64 bit-cast to u64, nanoseconds: u32).
// =============================================================================

template <> class CompoundEncoder<DataType::kTime> {
public:
    CompoundEncoder() = default;

    void reset() noexcept {
        seconds_.reset();
        nanoseconds_.reset();
        row_count_ = 0;
    }

    void reserve(size_t expected_rows) {
        seconds_.reserve(expected_rows);
        nanoseconds_.reserve(expected_rows);
    }

    void add(int64_t seconds, uint32_t nanoseconds) noexcept {
        uint64_t sec_bits;
        std::memcpy(&sec_bits, &seconds, sizeof(sec_bits));
        seconds_.add(sec_bits);
        nanoseconds_.add(nanoseconds);
        ++row_count_;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }

    void finish_into(std::string* dst) const {
        const std::string seconds = seconds_.finish().data;
        const std::string nanoseconds = nanoseconds_.finish().data;
        detail::write_compound_units<2>(
            {{{DataType::kInt64, seconds}, {DataType::kUint32, nanoseconds}}}, dst);
    }

    [[nodiscard]] EncodeResult finish() const {
        EncodeResult r;
        r.row_count = row_count_;
        finish_into(&r.data);
        return r;
    }

private:
    RawEncoder<8> seconds_;
    RawEncoder<4> nanoseconds_;
    size_t row_count_ = 0;
};

// =============================================================================
// CompoundDecoder<DataType::kTime>
// =============================================================================

template <> class CompoundDecoder<DataType::kTime> {
public:
    CompoundDecoder() = default;

    [[nodiscard]] bool parse(std::string_view input) noexcept {
        std::array<std::string_view, 2> units;
        if (!detail::parse_compound_units<2>(
                input, {DataType::kInt64, DataType::kUint32}, &units, &bytes_consumed_))
            return false;
        if (!seconds_.parse(units[0])) {
            return false;
        }
        if (!nanoseconds_.parse(units[1])) {
            return false;
        }
        if (seconds_.bytes_consumed() != units[0].size()) {
            return false;
        }
        bytes_consumed_ = bytes_consumed_ - units[1].size() + nanoseconds_.bytes_consumed();

        if (seconds_.row_count() != nanoseconds_.row_count()) {
            return false;
        }

        row_count_ = seconds_.row_count();
        return true;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }
    [[nodiscard]] size_t bytes_consumed() const noexcept { return bytes_consumed_; }

    [[nodiscard]] int64_t seconds(size_t i) const noexcept {
        uint64_t bits = seconds_.get(i);
        int64_t result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    [[nodiscard]] uint32_t nanoseconds(size_t i) const noexcept { return nanoseconds_.get(i); }

private:
    RawDecoder<8> seconds_;
    RawDecoder<4> nanoseconds_;
    size_t row_count_ = 0;
    size_t bytes_consumed_ = 0;
};

// =============================================================================
// CompoundEncoder<DataType::kVersion>
//
// Encodes Version as (major: u64, minor: u64).
// =============================================================================

template <> class CompoundEncoder<DataType::kVersion> {
public:
    CompoundEncoder() = default;

    void reset() noexcept {
        major_.reset();
        minor_.reset();
        row_count_ = 0;
    }

    void reserve(size_t expected_rows) {
        major_.reserve(expected_rows);
        minor_.reserve(expected_rows);
    }

    void add(uint64_t major, uint64_t minor) noexcept {
        major_.add(major);
        minor_.add(minor);
        ++row_count_;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }

    void finish_into(std::string* dst) const {
        const std::string major = major_.finish().data;
        const std::string minor = minor_.finish().data;
        detail::write_compound_units<2>({{{DataType::kUint64, major}, {DataType::kUint64, minor}}},
                                        dst);
    }

    [[nodiscard]] EncodeResult finish() const {
        EncodeResult r;
        r.row_count = row_count_;
        finish_into(&r.data);
        return r;
    }

private:
    RawEncoder<8> major_;
    RawEncoder<8> minor_;
    size_t row_count_ = 0;
};

// =============================================================================
// CompoundDecoder<DataType::kVersion>
// =============================================================================

template <> class CompoundDecoder<DataType::kVersion> {
public:
    CompoundDecoder() = default;

    [[nodiscard]] bool parse(std::string_view input) noexcept {
        std::array<std::string_view, 2> units;
        if (!detail::parse_compound_units<2>(
                input, {DataType::kUint64, DataType::kUint64}, &units, &bytes_consumed_))
            return false;
        if (!major_.parse(units[0])) {
            return false;
        }
        if (!minor_.parse(units[1])) {
            return false;
        }
        if (major_.bytes_consumed() != units[0].size()) {
            return false;
        }
        bytes_consumed_ = bytes_consumed_ - units[1].size() + minor_.bytes_consumed();

        if (major_.row_count() != minor_.row_count()) {
            return false;
        }

        row_count_ = major_.row_count();
        return true;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }
    [[nodiscard]] size_t bytes_consumed() const noexcept { return bytes_consumed_; }

    [[nodiscard]] uint64_t major(size_t i) const noexcept { return major_.get(i); }
    [[nodiscard]] uint64_t minor(size_t i) const noexcept { return minor_.get(i); }

private:
    RawDecoder<8> major_;
    RawDecoder<8> minor_;
    size_t row_count_ = 0;
    size_t bytes_consumed_ = 0;
};

// =============================================================================
// Type aliases for convenience and backward compatibility.
// =============================================================================

using StringRefEncoder = CompoundEncoder<DataType::kString>;
using StringRefDecoder = CompoundDecoder<DataType::kString>;
using TimeEncoder = CompoundEncoder<DataType::kTime>;
using TimeDecoder = CompoundDecoder<DataType::kTime>;
using VersionEncoder = CompoundEncoder<DataType::kVersion>;
using VersionDecoder = CompoundDecoder<DataType::kVersion>;

} // namespace pl::sstv2::pattern
