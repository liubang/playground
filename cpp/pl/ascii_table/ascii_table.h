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

#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <folly/Expected.h>
#include <folly/dynamic.h>
#include <folly/json.h>

namespace pl {

namespace ascii_table {
// 错误类型
enum class TableError {
    INVALID_ROW_SIZE,
    INVALID_COLUMN_INDEX,
    INVALID_ROW_INDEX,
    FORMATTING_ERROR
};

// 对齐方式
enum class Alignment : uint8_t { LEFT = 0, CENTER = 1, RIGHT = 2 };

// 颜色支持
enum class Color : uint8_t {
    NONE = 0,
    BLACK = 30,
    RED = 31,
    GREEN = 32,
    YELLOW = 33,
    BLUE = 34,
    MAGENTA = 35,
    CYAN = 36,
    WHITE = 37,
    BRIGHT_BLACK = 90,
    BRIGHT_RED = 91,
    BRIGHT_GREEN = 92,
    BRIGHT_YELLOW = 93,
    BRIGHT_BLUE = 94,
    BRIGHT_MAGENTA = 95,
    BRIGHT_CYAN = 96,
    BRIGHT_WHITE = 97
};

// 单元格样式
struct CellStyle {
    Alignment alignment{Alignment::LEFT};
    Color fg_color{Color::NONE};
    Color bg_color{Color::NONE};
    bool bold{false};
    bool italic{false};
    bool underline{false};

    CellStyle() = default;

    CellStyle(Alignment align) : alignment(align) {}

    CellStyle(Color fg) : fg_color(fg) {}

    CellStyle(Alignment align, Color fg, bool is_bold = false)
        : alignment(align), fg_color(fg), bold(is_bold) {}
};

// 边框字符集
struct BorderChars {
    char top_left;
    char top_right;
    char bottom_left;
    char bottom_right;
    char horizontal;
    char vertical;
    char cross;
    char top_tee;
    char bottom_tee;
    char left_tee;
    char right_tee;

    static BorderChars ascii() { return {'+', '+', '+', '+', '-', '|', '+', '+', '+', '+', '+'}; }
};

// 表格配置
struct TableConfig {
    bool show_header{true};
    bool show_row_numbers{false};
    size_t padding{1};
    size_t max_column_width{50};
    std::string title;
    CellStyle header_style{Alignment::CENTER, Color::CYAN, true};
    CellStyle default_style{Alignment::LEFT};
    std::map<size_t, CellStyle> column_styles;
};
} // namespace ascii_table

class ASCIITable {
public:
    using Void = folly::Unit;

    // 构造函数
    explicit ASCIITable(ascii_table::TableConfig config = {}) : config_(std::move(config)) {
        // 预分配一些空间以提高性能
        data_.reserve(16);
        column_widths_.reserve(16);
    }

    template <typename Container>
    folly::Expected<Void, ascii_table::TableError> setHeader(const Container& header) {
        std::vector<std::string> header_row;
        header_row.reserve(header.size());

        for (const auto& item : header) {
            header_row.emplace_back(folly::to<std::string>(item));
        }

        if (header_row.empty()) {
            return folly::makeUnexpected(ascii_table::TableError::INVALID_ROW_SIZE);
        }

        header_ = std::move(header_row);
        num_columns_ = header_->size();
        column_widths_.resize(num_columns_, 0);
        updateColumnWidths(*header_);

        return folly::unit;
    }

    // 添加行（支持可变参数）
    template <typename... Args>
    folly::Expected<Void, ascii_table::TableError> addRow(Args&&... args) {
        static_assert(sizeof...(args) > 0, "Row cannot be empty");

        std::vector<std::string> row;
        row.reserve(sizeof...(args));

        // 使用 Folly 的 to<> 进行类型转换
        (row.emplace_back(folly::to<std::string>(std::forward<Args>(args))), ...);

        return addRowImpl(std::move(row));
    }

    // 添加行（从容器）
    template <typename Container>
    folly::Expected<Void, ascii_table::TableError> addRow(const Container& row_data) {
        std::vector<std::string> row;
        row.reserve(row_data.size());

        for (const auto& item : row_data) {
            row.emplace_back(folly::to<std::string>(item));
        }

        return addRowImpl(std::move(row));
    }

    // 从 Folly Dynamic 添加数据
    folly::Expected<Void, ascii_table::TableError> addFromDynamic(const folly::dynamic& data) {
        if (!data.isArray()) {
            return folly::makeUnexpected(ascii_table::TableError::FORMATTING_ERROR);
        }

        for (const auto& row : data) {
            if (!row.isArray()) {
                return folly::makeUnexpected(ascii_table::TableError::FORMATTING_ERROR);
            }
            std::vector<std::string> row_data;
            row_data.reserve(row.size());

            for (const auto& cell : row) {
                row_data.emplace_back(folly::toJson(cell));
            }

            auto result = addRowImpl(std::move(row_data));
            if (!result) {
                return result;
            }
        }

        return folly::unit;
    }

    // 设置单元格样式
    void setCellStyle(size_t row, size_t col, const ascii_table::CellStyle& style) {
        cell_styles_[std::make_pair(row, col)] = style;
    }

    // 设置列样式
    void setColumnStyle(size_t col, const ascii_table::CellStyle& style) {
        config_.column_styles[col] = style;
    }

    // 设置行样式
    void setRowStyle(size_t row, const ascii_table::CellStyle& style) {
        for (size_t col = 0; col < num_columns_; ++col) {
            setCellStyle(row, col, style);
        }
    }

    // 生成表格字符串（使用 Folly 的高效字符串操作）
    [[nodiscard]] std::string toString() const {
        if (!header_ && data_.empty()) {
            return std::string{};
        }

        // 使用 Folly 的 IOBuf 可能更高效，但这里为了简单使用 fbstring
        std::vector<std::string> lines;
        lines.reserve(data_.size() + 10); // 预分配空间

        const auto border_chars = getBorderChars();

        // 添加标题
        if (!config_.title.empty()) {
            addTitle(lines, border_chars);
        }

        // 添加顶部边框
        lines.emplace_back(createBorderLine(border_chars, true, false));

        // 添加表头
        if (header_ && config_.show_header) {
            lines.emplace_back(createDataLine(*header_, config_.header_style, true));
            lines.emplace_back(createBorderLine(border_chars, false, false));
        }

        // 添加数据行
        for (size_t i = 0; i < data_.size(); ++i) {
            lines.emplace_back(createDataLine(data_[i], config_.default_style, false, i));
        }

        // 添加底部边框
        lines.emplace_back(createBorderLine(border_chars, false, true));

        // 使用 Folly 的 join 高效连接
        return folly::join("\n", lines);
    }

    // 直接打印到流
    void print(std::ostream& os = std::cout) const { os << toString() << std::endl; }

    // 获取统计信息
    struct Stats {
        size_t rows;
        size_t columns;
        size_t total_cells;
        size_t memory_usage_bytes;
    };

    [[nodiscard]] Stats getStats() const {
        size_t memory = 0;

        // 计算数据内存使用
        for (const auto& row : data_) {
            for (const auto& cell : row) {
                memory += cell.capacity();
            }
        }

        if (header_) {
            for (const auto& cell : *header_) {
                memory += cell.capacity();
            }
        }

        return Stats{.rows = data_.size(),
                     .columns = num_columns_,
                     .total_cells = data_.size() * num_columns_,
                     .memory_usage_bytes = memory};
    }

    // 清空表格
    void clear() {
        data_.clear();
        header_.reset();
        cell_styles_.clear();
        column_widths_.clear();
        num_columns_ = 0;
    }

    // 获取配置的引用，允许动态修改
    ascii_table::TableConfig& getConfig() { return config_; }

    [[nodiscard]] const ascii_table::TableConfig& getConfig() const { return config_; }

private:
    folly::Expected<Void, ascii_table::TableError> addRowImpl(std::vector<std::string> row) {
        if (header_ && row.size() != num_columns_) {
            return folly::makeUnexpected(ascii_table::TableError::INVALID_ROW_SIZE);
        }

        if (!header_ && !data_.empty() && row.size() != num_columns_) {
            return folly::makeUnexpected(ascii_table::TableError::INVALID_ROW_SIZE);
        }

        if (!header_ && num_columns_ == 0) {
            num_columns_ = row.size();
            column_widths_.resize(num_columns_, 0);
        }

        updateColumnWidths(row);
        data_.emplace_back(std::move(row));

        return folly::unit;
    }

    void updateColumnWidths(const std::vector<std::string>& row) {
        for (size_t i = 0; i < row.size() && i < column_widths_.size(); ++i) {
            // 使用 UTF-8 长度而不是字节长度
            size_t display_width = getDisplayWidth(row[i]);
            column_widths_[i] =
                std::max(column_widths_[i], std::min(display_width, config_.max_column_width));
        }
    }

    // 获取字符串显示宽度（处理 UTF-8）
    [[nodiscard]] size_t getDisplayWidth(const std::string& str) const {
        // 简化版本，实际应该处理 Unicode 字符宽度
        return str.size();
    }

    [[nodiscard]] ascii_table::BorderChars getBorderChars() const {
        return ascii_table::BorderChars::ascii();
    }

    void addTitle(std::vector<std::string>& lines, const ascii_table::BorderChars& chars) const {
        size_t table_width = calculateTableWidth();
        std::string title_line = folly::sformat(
            "{}{}{}", chars.vertical, centerText(config_.title, table_width - 2), chars.vertical);

        std::string title_border = std::string(1, chars.top_left) +
                                   std::string(table_width - 2, chars.horizontal) +
                                   std::string(1, chars.top_right);

        lines.emplace_back(std::move(title_border));
        lines.emplace_back(std::move(title_line));
    }

    [[nodiscard]] std::string createBorderLine(const ascii_table::BorderChars& chars,
                                               bool is_top,
                                               bool is_bottom) const {
        std::string line;
        line.reserve(calculateTableWidth());

        // 左边框
        if (is_top) {
            line += chars.top_left;
        } else if (is_bottom) {
            line += chars.bottom_left;
        } else {
            line += chars.left_tee;
        }

        // 列分隔
        for (size_t i = 0; i < num_columns_; ++i) {
            line += std::string(column_widths_[i] + 2 * config_.padding, chars.horizontal);

            if (i < num_columns_ - 1) {
                if (is_top) {
                    line += chars.top_tee;
                } else if (is_bottom) {
                    line += chars.bottom_tee;
                } else {
                    line += chars.cross;
                }
            }
        }

        // 右边框
        if (is_top) {
            line += chars.top_right;
        } else if (is_bottom) {
            line += chars.bottom_right;
        } else {
            line += chars.right_tee;
        }

        return line;
    }

    [[nodiscard]] std::string createDataLine(const std::vector<std::string>& row,
                                             const ascii_table::CellStyle& default_style,
                                             bool is_header,
                                             size_t row_index = 0) const {
        std::string line;
        line.reserve(calculateTableWidth());

        line += getBorderChars().vertical;

        for (size_t i = 0; i < num_columns_; ++i) {
            std::string cell_content;

            if (i < row.size()) {
                cell_content = row[i];
            }

            // 获取单元格样式
            ascii_table::CellStyle cell_style =
                getCellStyle(row_index, i, default_style, is_header);

            // 格式化单元格内容
            std::string formatted_cell = formatCell(cell_content, column_widths_[i], cell_style);

            line += std::string(config_.padding, ' ');
            line += formatted_cell;
            line += std::string(config_.padding, ' ');

            if (i < num_columns_ - 1) {
                line += getBorderChars().vertical;
            }
        }

        line += getBorderChars().vertical;

        return line;
    }

    [[nodiscard]] ascii_table::CellStyle getCellStyle(size_t row,
                                                      size_t col,
                                                      const ascii_table::CellStyle& default_style,
                                                      bool is_header) const {
        // 优先级：单元格样式 > 列样式 > 默认样式
        ascii_table::CellStyle style = default_style;

        if (!is_header) {
            auto col_style_it = config_.column_styles.find(col);
            if (col_style_it != config_.column_styles.end()) {
                style = col_style_it->second;
            }
        }

        auto cell_style_it = cell_styles_.find(std::make_pair(row, col));
        if (cell_style_it != cell_styles_.end()) {
            style = cell_style_it->second;
        }

        return style;
    }

    [[nodiscard]] std::string formatCell(const std::string& content,
                                         size_t width,
                                         const ascii_table::CellStyle& style) const {
        std::string result = content;

        // 截断过长的内容
        if (result.size() > width) {
            result = result.substr(0, width - 3) + "...";
        }

        // 应用对齐
        result = alignText(result, width, style.alignment);

        // 应用颜色和样式
        result = applyStyle(result, style);

        return result;
    }

    [[nodiscard]] std::string alignText(const std::string& text,
                                        size_t width,
                                        ascii_table::Alignment align) const {
        if (text.size() >= width) {
            return text;
        }

        size_t padding = width - text.size();

        switch (align) {
        case ascii_table::Alignment::CENTER:
        {
            size_t left_pad = padding / 2;
            size_t right_pad = padding - left_pad;
            return std::string(left_pad, ' ') + text + std::string(right_pad, ' ');
        }
        case ascii_table::Alignment::RIGHT:
            return std::string(padding, ' ') + text;
        case ascii_table::Alignment::LEFT:
        default:
            return text + std::string(padding, ' ');
        }
    }

    [[nodiscard]] std::string applyStyle(const std::string& text,
                                         const ascii_table::CellStyle& style) const {
        if (style.fg_color == ascii_table::Color::NONE &&
            style.bg_color == ascii_table::Color::NONE && !style.bold && !style.italic &&
            !style.underline) {
            return text;
        }

        std::string result = "\033[";
        std::vector<std::string> codes;

        if (style.bold)
            codes.emplace_back("1");
        if (style.italic)
            codes.emplace_back("3");
        if (style.underline)
            codes.emplace_back("4");
        if (style.fg_color != ascii_table::Color::NONE) {
            codes.emplace_back(folly::to<std::string>(static_cast<int>(style.fg_color)));
        }
        if (style.bg_color != ascii_table::Color::NONE) {
            codes.emplace_back(folly::to<std::string>(static_cast<int>(style.bg_color) + 10));
        }

        result += folly::join(";", codes);
        result += "m";
        result += text;
        result += "\033[0m";

        return result;
    }

    [[nodiscard]] std::string centerText(const std::string& text, size_t width) const {
        return alignText(text, width, ascii_table::Alignment::CENTER);
    }

    [[nodiscard]] size_t calculateTableWidth() const {
        if (num_columns_ == 0)
            return 0;

        size_t width = 0;

        // 列宽 + padding
        for (size_t col_width : column_widths_) {
            width += col_width + 2 * config_.padding;
        }

        // 边框
        width += num_columns_ + 1; // 垂直边框

        return width;
    }

private:
    ascii_table::TableConfig config_;
    std::optional<std::vector<std::string>> header_;
    std::vector<std::vector<std::string>> data_;
    std::vector<size_t> column_widths_;
    std::map<std::pair<size_t, size_t>, ascii_table::CellStyle> cell_styles_;
    size_t num_columns_{0};
};

} // namespace pl
