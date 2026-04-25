// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/07/27 22:22

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace pl::pretty {

enum class CellType : uint8_t {
    CT_NONE = 0,
    CT_STRING = 1,
    CT_SEP = 2,
};

struct Cell {
    Cell() = default;
    Cell(CellType type, std::string value) : t(type), val(std::move(value)) {}

    [[nodiscard]] bool valid() const { return t != CellType::CT_NONE; }

    CellType t{CellType::CT_NONE};
    std::string val;
};

using Row = std::vector<Cell>;

class Pretty {
public:
    explicit Pretty(const std::vector<std::string>& headers);

    Pretty(const Pretty&) = delete;
    Pretty(Pretty&&) = default;
    Pretty& operator=(const Pretty&) = delete;
    Pretty& operator=(Pretty&&) = default;

    Pretty& next();

    /**
     * @brief add sep line
     *
     * @param sep string
     * @return Pretty&
     */
    Pretty& add_sep(const std::string& sep);

    Pretty& add_row(const std::vector<std::string>& row);

    Pretty& set_show_sep(bool show_sep) {
        show_sep_ = show_sep;
        return *this;
    }

    void render() const;
    void render(std::ostream& out) const;
    [[nodiscard]] std::string str() const;

private:
    Pretty& add_cell(CellType t, const std::string& val);

    void print_header(std::ostream& out, uint32_t len, char sep) const;

    void print_header_line(std::ostream& out, uint32_t len, char sep) const;

    void print_line(std::ostream& out, const Row& cells) const;

    [[nodiscard]] std::string pad_right(const std::string& input,
                                        size_t total_length,
                                        char pad_char = ' ') const;

private:
    std::vector<Row> lines_;                // 行信息
    std::vector<uint32_t> cell_max_length_; // 每一列最大长度
    uint32_t maxcell_per_line_{0};          // 每一行最多有多少列
    uint32_t cell_idx_{0};                  // 当前列的下标
    bool show_sep_{true};                   // 是否显示分隔符
};

} // namespace pl::pretty
