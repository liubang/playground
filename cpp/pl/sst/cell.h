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

#include <cassert>
#include <cstdint>
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

struct CellKey {
    std::string_view rowkey;
    std::string_view cf;
    std::string_view col;
    uint64_t timestamp;
    CellType cell_type;

    std::string encode() const {
        std::string dist;
        dist.append(rowkey);
        dist.append(cf);
        dist.append(1, '\0');
        dist.append(col);
        dist.append(1, '\0');
        dist.append(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));
        dist.append(reinterpret_cast<const char*>(&cell_type), sizeof(cell_type));
        return dist;
    }

    int compare(const ComparatorRef& rowkey_comparator, const CellKey& other) const {
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
        if (timestamp > other.timestamp) {
            return -1;
        }
        if (timestamp < other.timestamp) {
            return 1;
        }
        if (cell_type > other.cell_type) {
            return 1;
        }
        if (cell_type < other.cell_type) {
            return -1;
        }
        return 0;
    }
};

class Cell {
public:
    Cell() = default;

    std::string_view rowkey() const { return cell_key_.rowkey; }

    std::string_view cf() const { return cell_key_.cf; }

    std::string_view col() const { return cell_key_.col; }

    uint64_t timestamp() const { return cell_key_.timestamp; }

    CellType cellType() const { return cell_key_.cell_type; }

    const CellKey& cellKey() const { return cell_key_; }

    std::string_view value() const { return value_; }

private:
    CellKey cell_key_;
    std::string_view value_;
};

} // namespace pl
