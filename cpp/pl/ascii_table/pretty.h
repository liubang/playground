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

// Ascii-table rendering with a pipeline API.
//
//   auto t = header("Name", "Age")
//          | row("alice",  "30")
//          | row("bob",    "25")
//          | sep('-')
//          | row("charlie","35");
//   std::cout << t << '\n';
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace pl::pretty {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class Table;

// ---------------------------------------------------------------------------
// Lightweight tag types — zero overhead carriers
// ---------------------------------------------------------------------------

struct Header {
    std::vector<std::string> cols;
};

struct DataRow {
    std::vector<std::string> cols;
};

struct SepLine {
    char32_t c;
};

// ---------------------------------------------------------------------------
// Factory functions (CTAD-style: types are deduced, no angle brackets)
// ---------------------------------------------------------------------------

template <typename... Args> auto header(Args&&... args) -> Header {
    return {{std::string(std::forward<Args>(args))...}};
}

template <typename... Args> auto row(Args&&... args) -> DataRow {
    return {{std::string(std::forward<Args>(args))...}};
}

inline auto sep(char32_t c = U'-') -> SepLine {
    return {c};
}

// ---------------------------------------------------------------------------
// Pipeline operators — the "pipe" composes tags into a Table
// ---------------------------------------------------------------------------

/// Start a table: Header | DataRow → Table
Table operator|(Header h, DataRow r);

/// Start a table with a separator right after the header (rare)
Table operator|(Header h, SepLine s);

/// Append a data row to an existing table (rvalue — moves)
Table operator|(Table&& t, DataRow r);

/// Append a separator to an existing table (rvalue — moves)
Table operator|(Table&& t, SepLine s);

/// Append a data row (lvalue — copies the table)
Table operator|(const Table& t, DataRow r);

/// Append a separator (lvalue — copies the table)
Table operator|(const Table& t, SepLine s);

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

class Table {
public:
    Table() = default;

    // ---- imperative API (alternative to the pipeline) ----

    auto header(std::vector<std::string> cols) -> Table&;
    auto data(std::vector<std::string> cols) -> Table&;
    auto sep(char32_t c = U'-') -> Table&;

    /// Control borders.  Default is auto-detect from TTY.
    auto borders(bool on) -> Table& {
        borders_ = on;
        bordersExplicit_ = true;
        return *this;
    }

    // ---- output ----

    /// Render to an arbitrary ostream.
    void render(std::ostream& out) const;

    /// Render to std::cout.
    void render() const;

    /// Return the rendered table as a string.
    [[nodiscard]] auto str() const -> std::string;

    /// Convenience: std::cout << t;
    friend auto operator<<(std::ostream& os, const Table& t) -> std::ostream& {
        t.render(os);
        return os;
    }

private:
    enum class RowKind : uint8_t { kData, kSep };

    struct Row {
        std::vector<std::string> cols;
        RowKind kind = RowKind::kData;
        char32_t sepChar = U'-';
    };

    std::vector<Row> rows_;
    std::size_t maxCols_ = 0;
    bool borders_ = true;
    bool bordersExplicit_ = false;

    // internal helpers
    void pushHeader(std::vector<std::string> cols);
    void pushData(std::vector<std::string> cols);
    void pushSep(char32_t c);

    [[nodiscard]] auto colWidths() const -> std::vector<std::size_t>;
    [[nodiscard]] static auto totalWidth(const std::vector<std::size_t>& widths,
                                         std::size_t maxCols) -> std::size_t;

    static void printBorder(std::ostream& out, std::size_t total, char32_t ch);
    void printData(std::ostream& out,
                   const std::vector<std::string>& cols,
                   const std::vector<std::size_t>& widths) const;
    static auto padRight(std::string s, std::size_t width, char pad = ' ') -> std::string;
};

} // namespace pl::pretty
