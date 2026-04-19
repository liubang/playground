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

#include "pretty.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace pl::pretty {

Pretty::Pretty(const std::vector<std::string>& headers) {
    maxcell_per_line_ = static_cast<uint32_t>(headers.size());
    cell_max_length_.resize(maxcell_per_line_, 0);
    add_row(headers);
}

Pretty& Pretty::next() {
    cell_idx_ = 0;
    lines_.emplace_back();
    lines_.back().reserve(maxcell_per_line_);
    return *this;
}

Pretty& Pretty::add_sep(const std::string& sep) {
    if (sep.empty()) {
        throw std::invalid_argument("Separator must not be empty.");
    }
    return next().add_cell(CellType::CT_SEP, sep);
}

Pretty& Pretty::add_row(const std::vector<std::string>& vals) {
    if (vals.size() != maxcell_per_line_) {
        throw std::out_of_range("Row cell count must match the table header count.");
    }
    next();
    for (const auto& val : vals) {
        add_cell(CellType::CT_STRING, val);
    }
    return *this;
}

Pretty& Pretty::add_cell(CellType t, const std::string& val) {
    auto lines = lines_.size();
    auto& cells = lines_[lines - 1];
    if (cell_idx_ >= maxcell_per_line_) {
        throw std::out_of_range("Too many cells in this line.");
    }
    if (cell_max_length_[cell_idx_] < val.size()) {
        cell_max_length_[cell_idx_] = static_cast<uint32_t>(val.size());
    }
    cells.push_back(std::make_unique<Cell>(t, val));
    ++cell_idx_;
    return *this;
}

void Pretty::render() const { render(std::cout); }

void Pretty::render(std::ostream& out) const {
    auto len = maxcell_per_line_ * 3 + 1;
    for (const auto& mlen : cell_max_length_) {
        len += mlen;
    }
    if (show_sep_) {
        print_header(out, len, '=');
    } else if (!lines_.empty()) {
        print_line(out, lines_[0]);
    }
    for (size_t i = 1; i < lines_.size(); ++i) {
        if (lines_[i].size() > 0 && lines_[i][0]->t == CellType::CT_SEP) {
            if (show_sep_) {
                print_header_line(out, len, lines_[i][0]->val[0]);
            }
        } else {
            print_line(out, lines_[i]);
        }
    }
    if (show_sep_) {
        print_header_line(out, len, '=');
    }
}

std::string Pretty::str() const {
    std::ostringstream out;
    render(out);
    return out.str();
}

void Pretty::print_header(std::ostream& out, uint32_t len, char sep) const {
    if (lines_.empty()) {
        return;
    }
    print_header_line(out, len, sep);
    print_line(out, lines_[0]);
    print_header_line(out, len, sep);
}

void Pretty::print_header_line(std::ostream& out, uint32_t len, char sep) const {
    std::ostringstream oss;
    oss << "+" << std::string(len - 2, sep) << "+";
    out << oss.str() << '\n';
}

void Pretty::print_line(std::ostream& out, const std::vector<CellPtr>& cells) const {
    if (cells.empty()) {
        return;
    }
    std::ostringstream oss;
    if (show_sep_) {
        oss << "| ";
    }
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& cell = cells[i];
        auto max_len = cell_max_length_[i];
        if (!cell) {
            oss << pad_right("", max_len);
        } else {
            oss << pad_right(cell->val, max_len);
        }
        if (show_sep_ && i + 1 < cells.size()) {
            oss << " | ";
        } else if (!show_sep_ && i + 1 < cells.size()) {
            oss << " ";
        }
    }
    if (show_sep_) {
        oss << " |";
    }
    out << oss.str() << '\n';
}

[[nodiscard]] std::string Pretty::pad_right(const std::string& input,
                                            size_t total_length,
                                            char pad_char) const {
    if (input.size() >= total_length) {
        return input.substr(0, total_length);
    }
    std::string result;
    result.reserve(total_length);
    result.append(input);
    result.append(total_length - input.size(), pad_char);
    return result;
}

} // namespace pl::pretty
