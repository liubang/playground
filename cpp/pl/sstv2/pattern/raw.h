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

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/bits/bits.h"
#include "cpp/pl/sstv2/codec/varint.h"
#include "cpp/pl/sstv2/pattern/pattern.h"

namespace pl::sstv2::pattern {

// =============================================================================
// CellTraits<CellSize>: maps byte width to the natural unsigned integer type.
// For CellSize==16 there is no native scalar; we use raw bytes only.
// =============================================================================

namespace detail {

template <size_t CellSize> struct CellTraits {
    static_assert(CellSize == 1 || CellSize == 2 || CellSize == 4 || CellSize == 8 ||
                      CellSize == 16,
                  "CellSize must be 1, 2, 4, 8, or 16");
};

template <> struct CellTraits<1> {
    using type = uint8_t;
};
template <> struct CellTraits<2> {
    using type = uint16_t;
};
template <> struct CellTraits<4> {
    using type = uint32_t;
};
template <> struct CellTraits<8> {
    using type = uint64_t;
};
template <> struct CellTraits<16> {
    using type = void;
}; // no scalar type

template <size_t CellSize> using cell_type_t = typename CellTraits<CellSize>::type;

} // namespace detail

// =============================================================================
// RawEncoder<CellSize>: Pattern 0 encoder for fixed-size columns.
//
// Wire format:
//   uint8   pattern_id = 0
//   varint  row_count
//   uint8[] cells (each CellSize bytes, little-endian)
//
// Design:
//   - std::vector<uint8_t> as byte buffer (correct semantics, no SSO overhead).
//   - Single add() with if-constexpr dispatch; no SFINAE overload explosion.
//   - On LE platforms, scalar add is a single memcpy (no intermediate encode).
// =============================================================================

template <size_t CellSize> class RawEncoder {
    static_assert(CellSize == 1 || CellSize == 2 || CellSize == 4 || CellSize == 8 ||
                      CellSize == 16,
                  "CellSize must be 1, 2, 4, 8, or 16");

public:
    using cell_type = detail::cell_type_t<CellSize>;

    RawEncoder() = default;

    void reset() noexcept {
        buf_.clear();
        row_count_ = 0;
    }

    void reserve(size_t expected_rows) { buf_.reserve(expected_rows * CellSize); }

    // Primary: append raw bytes. Caller guarantees LE layout and exactly CellSize bytes.
    void add(const uint8_t* cell_bytes) noexcept {
        buf_.insert(buf_.end(), cell_bytes, cell_bytes + CellSize);
        ++row_count_;
    }

    // Scalar overload (CellSize <= 8): accepts the native unsigned integer type.
    // On LE platforms this compiles to a single memcpy; on BE it byte-swaps first.
    template <size_t S = CellSize>
        requires(S <= 8)
    void add(detail::cell_type_t<S> v) noexcept {
        if constexpr (CellSize == 1) {
            buf_.push_back(v);
        } else {
            auto le = Endian::little(v);
            const auto* p = reinterpret_cast<const uint8_t*>(&le);
            buf_.insert(buf_.end(), p, p + CellSize);
        }
        ++row_count_;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }
    [[nodiscard]] static constexpr size_t cell_size() noexcept { return CellSize; }

    // Serialize into an existing buffer (zero intermediate allocation).
    void finish_into(std::string* dst) const {
        const size_t header_max = 1 + 10; // pattern_id + varint
        dst->reserve(dst->size() + header_max + buf_.size());
        dst->push_back(static_cast<char>(static_cast<uint8_t>(PatternId::kRaw)));
        codec::encode_varint(row_count_, dst);
        dst->append(reinterpret_cast<const char*>(buf_.data()), buf_.size());
    }

    [[nodiscard]] EncodeResult finish() const {
        EncodeResult r;
        r.row_count = row_count_;
        finish_into(&r.data);
        return r;
    }

private:
    std::vector<uint8_t> buf_;
    size_t row_count_ = 0;
};

// =============================================================================
// RawDecoder<CellSize>: Pattern 0 decoder. Zero-copy O(1) random access.
//
// Holds a pointer into the parsed input — caller must keep input alive.
// =============================================================================

template <size_t CellSize> class RawDecoder {
    static_assert(CellSize == 1 || CellSize == 2 || CellSize == 4 || CellSize == 8 ||
                      CellSize == 16,
                  "CellSize must be 1, 2, 4, 8, or 16");

public:
    using cell_type = detail::cell_type_t<CellSize>;

    RawDecoder() = default;

    [[nodiscard]] bool parse(std::string_view input) noexcept {
        const auto* src = reinterpret_cast<const uint8_t*>(input.data());
        const size_t len = input.size();

        if (len < 1 || src[0] != static_cast<uint8_t>(PatternId::kRaw))
            return false;
        size_t pos = 1;

        uint64_t rc = 0;
        const size_t n = codec::decode_varint(src + pos, len - pos, &rc);
        if (n == 0)
            return false;
        pos += n;

        const size_t payload = static_cast<size_t>(rc) * CellSize;
        if (pos + payload > len)
            return false;

        cells_ = src + pos;
        row_count_ = static_cast<size_t>(rc);
        bytes_consumed_ = pos + payload;
        return true;
    }

    [[nodiscard]] size_t row_count() const noexcept { return row_count_; }
    [[nodiscard]] size_t bytes_consumed() const noexcept { return bytes_consumed_; }
    [[nodiscard]] static constexpr size_t cell_size() noexcept { return CellSize; }

    // Raw pointer to i-th cell (LE bytes).
    [[nodiscard]] const uint8_t* cell(size_t i) const noexcept {
        assert(i < row_count_);
        return cells_ + i * CellSize;
    }

    // Typed accessor (CellSize <= 8): returns host-order scalar.
    template <size_t S = CellSize>
        requires(S <= 8)
    [[nodiscard]] detail::cell_type_t<S> get(size_t i) const noexcept {
        if constexpr (CellSize == 1) {
            return cell(i)[0];
        } else {
            detail::cell_type_t<S> le;
            std::memcpy(&le, cell(i), CellSize);
            return Endian::little(le);
        }
    }

    // 16-byte accessor: copies into caller-provided buffer.
    template <size_t S = CellSize>
        requires(S == 16)
    void get(size_t i, uint8_t (&out)[16]) const noexcept {
        std::memcpy(out, cell(i), 16);
    }

private:
    const uint8_t* cells_ = nullptr;
    size_t row_count_ = 0;
    size_t bytes_consumed_ = 0;
};

} // namespace pl::sstv2::pattern
