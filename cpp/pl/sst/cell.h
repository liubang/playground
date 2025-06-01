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

#include "cpp/pl/arena/arena.h"
#include "cpp/pl/sst/comparator.h"
#include "cpp/pl/sst/encoding.h"

#include "cpp/pl/status/result.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace pl {

// clang-format off
enum class CellType : uint8_t {
    CT_DEL  = 0,
    CT_PUT  = 1,
    CT_READ = 2,
    CT_NONE = 3,
};
// clang-format on

class Cell;

using CellPtr = std::unique_ptr<Cell>;
using CellRef = std::shared_ptr<Cell>;
using CellVecRef = std::vector<CellRef>;

struct CellKey {
    std::string_view rowkey;
    std::string_view cf;
    std::string_view col;
    uint64_t timestamp;
    CellType cell_type;

    CellKey() = default;

    CellKey(std::string_view rowkey,
            std::string_view cf,
            std::string_view col,
            uint64_t ts,
            CellType type)
        : rowkey(rowkey), cf(cf), col(col), timestamp(ts), cell_type(type) {}

    void reset() {
        rowkey = {};
        cf = {};
        col = {};
        timestamp = 0;
        cell_type = CellType::CT_NONE;
    }

    [[nodiscard]] std::string encode() const {
        std::string result;
        // 预先计算空间，减少重分配
        const size_t estimated_size =
            rowkey.size() + cf.size() + col.size() + 2 + sizeof(uint64_t) + sizeof(uint8_t);
        result.reserve(estimated_size);

        result.append(rowkey);
        result.append(cf);
        result.append(1, '\0');
        result.append(col);
        result.append(1, '\0');
        encodeInt<uint64_t>(&result, timestamp);
        encodeInt<uint8_t>(&result, static_cast<uint8_t>(cell_type));
        return result;
    }

    Result<Void> decode(std::string_view encoded, uint32_t rowkey_len) {
        if (encoded.size() < rowkey_len + sizeof(uint64_t) + sizeof(uint8_t) + 2) {
            // 错误处理，数据长度不足
            reset();
            return makeError(StatusCode::kInvalidCell);
        }
        const char* data = encoded.data();
        const char* end = data + encoded.size();

        // decode rowkey
        rowkey = std::string_view(data, rowkey_len);
        data += rowkey_len;

        // decode cf
        const char* cf_start = data;
        const char* cf_end = static_cast<const char*>(std::memchr(cf_start, '\0', end - cf_start));
        if (UNLIKELY(cf_end == nullptr)) {
            reset();
            return makeError(StatusCode::kInvalidCell);
        }
        cf = std::string_view(cf_start, cf_end - cf_start);
        data = cf_end + 1;

        // decode col
        const char* col_start = data;
        const char* col_end =
            static_cast<const char*>(std::memchr(col_start, '\0', end - col_start));
        if (col_end == nullptr) {
            reset();
            return makeError(StatusCode::kInvalidCell);
        }
        col = std::string_view(col_start, col_end - col_start);
        data = col_end + 1;

        // decode timestamp
        if (end - data < static_cast<ptrdiff_t>(sizeof(uint64_t) + sizeof(uint8_t))) {
            reset();
            return makeError(StatusCode::kInvalidCell);
        }
        timestamp = decodeInt<uint64_t>(data);
        data += sizeof(uint64_t);

        // decode celltype
        cell_type = static_cast<CellType>(*data);

        RETURN_VOID;
    }

    [[nodiscard]] int compare(const ComparatorRef& rowkey_comparator, const CellKey& other) const {
        // rowkey 单独排序，用户可能会自定义rowkey comparator
        int ret = rowkey_comparator->compare(rowkey, other.rowkey);
        if (ret != 0) {
            return ret;
        }
        ret = cf.compare(other.cf);
        if (ret != 0) {
            return ret;
        }
        ret = col.compare(other.col);
        if (ret != 0) {
            return ret;
        }

        // 按时间戳倒序，时间戳越大的，数据越新，应该排在前面
        if (timestamp != other.timestamp) {
            return timestamp > other.timestamp ? -1 : 1;
        }

        // 直接比较枚举值
        if (cell_type != other.cell_type) {
            return cell_type > other.cell_type ? 1 : -1;
        }

        return 0;
    }

    // 添加相等比较运算符
    [[nodiscard]] bool operator==(const CellKey& other) const noexcept {
        return rowkey == other.rowkey && cf == other.cf && col == other.col &&
               timestamp == other.timestamp && cell_type == other.cell_type;
    }

    [[nodiscard]] bool operator!=(const CellKey& other) const noexcept { return !(*this == other); }
};

class Cell {
public:
    Cell() noexcept = default;

    Cell(CellType type,
         std::string_view rowkey,
         std::string_view cf,
         std::string_view col,
         std::string_view value,
         uint64_t timestamp)
        : cell_key_(rowkey, cf, col, timestamp, type), value_(value) {}

    Cell(std::string_view cell_key, uint32_t rowkey_size, std::string_view val) {
        cell_key_.decode(cell_key, rowkey_size);
        value_ = val;
    }

    // 移动构造函数和赋值运算符
    Cell(Cell&&) noexcept = default;
    Cell& operator=(Cell&&) noexcept = default;

    // 删除拷贝构造和赋值，避免意外的string_view拷贝问题
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;

    [[nodiscard]] CellPtr clone(Arena* arena) const {
        if (arena == nullptr) {
            return nullptr;
        }

        CellPtr new_cell = std::make_unique<Cell>();
        new_cell->cell_key_.cell_type = cell_type();
        new_cell->cell_key_.timestamp = timestamp();

        const size_t total_size = rowkey().size() + cf().size() + col().size() + value().size();
        if (total_size == 0) {
            return new_cell;
        }

        char* buf = static_cast<char*>(arena->allocate(total_size));
        if (buf == nullptr) {
            return nullptr;
        }

        char* current_buf = buf;
        if (LIKELY(!rowkey().empty())) {
            std::memcpy(current_buf, rowkey().data(), rowkey().size());
            new_cell->cell_key_.rowkey = {current_buf, rowkey().size()};
            current_buf += rowkey().size();
        }

        if (LIKELY(!cf().empty())) {
            std::memcpy(current_buf, cf().data(), cf().size());
            new_cell->cell_key_.cf = {current_buf, cf().size()};
            current_buf += cf().size();
        }

        if (LIKELY(!col().empty())) {
            std::memcpy(current_buf, col().data(), col().size());
            new_cell->cell_key_.col = {current_buf, col().size()};
            current_buf += col().size();
        }

        if (LIKELY(!value().empty())) {
            std::memcpy(current_buf, value().data(), value().size());
            new_cell->value_ = {current_buf, value().size()};
        }

        return new_cell;
    }

    [[nodiscard]] const CellKey& cellKey() const { return cell_key_; }
    [[nodiscard]] std::string_view rowkey() const { return cell_key_.rowkey; }
    [[nodiscard]] std::string_view cf() const { return cell_key_.cf; }
    [[nodiscard]] std::string_view col() const { return cell_key_.col; }
    [[nodiscard]] uint64_t timestamp() const { return cell_key_.timestamp; }
    [[nodiscard]] CellType cell_type() const { return cell_key_.cell_type; }
    [[nodiscard]] std::string_view value() const { return value_; }

    void reset() {
        cell_key_.reset();
        value_ = {};
    }

    // 添加实用方法
    [[nodiscard]] bool empty() const noexcept {
        return rowkey().empty() && cf().empty() && col().empty() && value().empty();
    }

    [[nodiscard]] size_t total_size() const noexcept {
        return rowkey().size() + cf().size() + col().size() + value().size();
    }

private:
    CellKey cell_key_;
    std::string_view value_;
};

} // namespace pl
