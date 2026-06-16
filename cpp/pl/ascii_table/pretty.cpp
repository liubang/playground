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
// Created: 2025/07/27 23:13

#include "pretty.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace pl::pretty {

// =============================================================================
// UTF-8 / display-width helpers (CJK-aware)
// =============================================================================

namespace {

/// Encode a code-point to its UTF-8 byte sequence.
auto toUtf8(char32_t cp) -> std::string {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

/// Decode one UTF-8 code-point.  Advances `it`.
auto decodeUtf8(std::string_view::iterator& it, std::string_view::iterator end) -> char32_t {
    auto c = static_cast<unsigned char>(*it++);
    if (c < 0x80)
        return c;

    std::size_t n = 0;
    if ((c & 0xE0) == 0xC0) {
        n = 1;
        c &= 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        c &= 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        c &= 0x07;
    } else {
        return 0xFFFD; // replacement character
    }

    char32_t cp = c;
    for (std::size_t i = 0; i < n; ++i) {
        if (it == end) return 0xFFFD; // truncated sequence
        auto byte = static_cast<unsigned char>(*it);
        if ((byte & 0xC0) != 0x80) return 0xFFFD; // invalid continuation byte
        ++it;
        cp = (cp << 6) | (byte & 0x3F);
    }
    // Reject overlong encodings and surrogates
    if ((n == 1 && cp < 0x80) ||
        (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF))
        return 0xFFFD;
    return cp;
}

/// Terminal display width of a code-point.  East-Asian / emoji → 2, ASCII → 1.
auto cpWidth(char32_t cp) noexcept -> int {
    if (cp < 0x20)
        return 0; // control characters
    if (cp < 0x7F)
        return 1;

    // Zero-width: combining marks, joiners, directional overrides
    if ((cp >= 0x0300 && cp <= 0x036F) ||   // Combining Diacritical Marks
        (cp >= 0x0483 && cp <= 0x0489) ||   // Cyrillic combining
        (cp >= 0x0591 && cp <= 0x05C7) ||   // Hebrew combining
        (cp >= 0x0610 && cp <= 0x065F) ||   // Arabic combining
        (cp >= 0x0670 && cp <= 0x0670) ||
        (cp >= 0x06D6 && cp <= 0x06ED) ||
        (cp >= 0x0711 && cp <= 0x0711) ||
        (cp >= 0x0730 && cp <= 0x074A) ||
        (cp >= 0x07A6 && cp <= 0x07B0) ||
        (cp >= 0x07EB && cp <= 0x07F3) ||
        (cp >= 0x0816 && cp <= 0x082D) ||
        (cp >= 0x0859 && cp <= 0x085B) ||
        (cp >= 0x08D4 && cp <= 0x0903) ||
        (cp >= 0x093A && cp <= 0x093C) ||   // Devanagari
        (cp >= 0x093E && cp <= 0x094F) ||
        (cp >= 0x0951 && cp <= 0x0957) ||
        (cp >= 0x0962 && cp <= 0x0963) ||
        (cp >= 0x0981 && cp <= 0x0983) ||   // Bengali
        (cp >= 0x09BC && cp <= 0x09BC) ||
        (cp >= 0x09BE && cp <= 0x09CD) ||
        (cp >= 0x09D7 && cp <= 0x09D7) ||
        (cp >= 0x09E2 && cp <= 0x09E3) ||
        (cp >= 0x0A01 && cp <= 0x0A03) ||   // Gurmukhi
        (cp >= 0x0A3C && cp <= 0x0A3C) ||
        (cp >= 0x0A3E && cp <= 0x0A4D) ||
        (cp >= 0x0A51 && cp <= 0x0A51) ||
        (cp >= 0x0A70 && cp <= 0x0A71) ||
        (cp >= 0x0A75 && cp <= 0x0A75) ||
        (cp >= 0x0A81 && cp <= 0x0A83) ||   // Gujarati
        (cp >= 0x0ABC && cp <= 0x0ACD) ||
        (cp >= 0x0AE2 && cp <= 0x0AE3) ||
        (cp >= 0x0B01 && cp <= 0x0B03) ||   // Oriya
        (cp >= 0x0B3C && cp <= 0x0B3C) ||
        (cp >= 0x0B3E && cp <= 0x0B4D) ||
        (cp >= 0x0B56 && cp <= 0x0B57) ||
        (cp >= 0x0B62 && cp <= 0x0B63) ||
        (cp >= 0x0B82 && cp <= 0x0B82) ||   // Tamil
        (cp >= 0x0BBE && cp <= 0x0BCD) ||
        (cp >= 0x0BD7 && cp <= 0x0BD7) ||
        (cp >= 0x0C00 && cp <= 0x0C03) ||   // Telugu
        (cp >= 0x0C3E && cp <= 0x0C4D) ||
        (cp >= 0x0C55 && cp <= 0x0C56) ||
        (cp >= 0x0C62 && cp <= 0x0C63) ||
        (cp >= 0x0C81 && cp <= 0x0C83) ||   // Kannada
        (cp >= 0x0CBC && cp <= 0x0CCD) ||
        (cp >= 0x0CD5 && cp <= 0x0CD6) ||
        (cp >= 0x0CE2 && cp <= 0x0CE3) ||
        (cp >= 0x0D01 && cp <= 0x0D03) ||   // Malayalam
        (cp >= 0x0D3E && cp <= 0x0D4D) ||
        (cp >= 0x0D57 && cp <= 0x0D57) ||
        (cp >= 0x0D62 && cp <= 0x0D63) ||
        (cp >= 0x0D82 && cp <= 0x0D83) ||   // Sinhala
        (cp >= 0x0DCA && cp <= 0x0DDF) ||
        (cp >= 0x0DF2 && cp <= 0x0DF3) ||
        (cp >= 0x0E31 && cp <= 0x0E3A) ||   // Thai
        (cp >= 0x0E47 && cp <= 0x0E4E) ||
        (cp >= 0x0EB1 && cp <= 0x0EB9) ||   // Lao
        (cp >= 0x0EBB && cp <= 0x0EBC) ||
        (cp >= 0x0EC8 && cp <= 0x0ECD) ||
        (cp >= 0x0F18 && cp <= 0x0F19) ||   // Tibetan
        (cp >= 0x0F35 && cp <= 0x0F35) ||
        (cp >= 0x0F37 && cp <= 0x0F37) ||
        (cp >= 0x0F39 && cp <= 0x0F39) ||
        (cp >= 0x0F3E && cp <= 0x0F3F) ||
        (cp >= 0x0F71 && cp <= 0x0F84) ||
        (cp >= 0x0F86 && cp <= 0x0F87) ||
        (cp >= 0x0F8D && cp <= 0x0FBC) ||
        (cp >= 0x0FC6 && cp <= 0x0FC6) ||
        (cp >= 0x102B && cp <= 0x103E) ||   // SE Asian
        (cp >= 0x1056 && cp <= 0x1064) ||
        (cp >= 0x1067 && cp <= 0x106D) ||
        (cp >= 0x1071 && cp <= 0x1074) ||
        (cp >= 0x1082 && cp <= 0x108D) ||
        (cp >= 0x108F && cp <= 0x108F) ||
        (cp >= 0x109A && cp <= 0x109D) ||
        (cp >= 0x135D && cp <= 0x135F) ||   // Ethiopic
        (cp >= 0x1712 && cp <= 0x1714) ||   // Philippine scripts
        (cp >= 0x1732 && cp <= 0x1734) ||
        (cp >= 0x1752 && cp <= 0x1753) ||
        (cp >= 0x1772 && cp <= 0x1773) ||
        (cp >= 0x17B4 && cp <= 0x17D3) ||   // Khmer
        (cp >= 0x17DD && cp <= 0x17DD) ||
        (cp >= 0x180B && cp <= 0x180D) ||   // Mongolian
        (cp >= 0x1885 && cp <= 0x1886) ||
        (cp >= 0x18A9 && cp <= 0x18A9) ||
        (cp >= 0x1920 && cp <= 0x193B) ||   // Limbu
        (cp >= 0x1A17 && cp <= 0x1A1B) ||   // SE Asian
        (cp >= 0x1A55 && cp <= 0x1A7C) ||
        (cp >= 0x1A7F && cp <= 0x1A7F) ||
        (cp >= 0x1AB0 && cp <= 0x1ABE) ||   // Comb Diacriticals Ext
        (cp >= 0x1B00 && cp <= 0x1B04) ||   // Balinese
        (cp >= 0x1B34 && cp <= 0x1B44) ||
        (cp >= 0x1B6B && cp <= 0x1B73) ||
        (cp >= 0x1B80 && cp <= 0x1B82) ||   // Sundanese
        (cp >= 0x1BA1 && cp <= 0x1BAD) ||
        (cp >= 0x1BE6 && cp <= 0x1BF3) ||   // Batak
        (cp >= 0x1C24 && cp <= 0x1C37) ||   // Lepcha
        (cp >= 0x1CD0 && cp <= 0x1CE8) ||   // Vedic
        (cp >= 0x1CED && cp <= 0x1CED) ||
        (cp >= 0x1CF2 && cp <= 0x1CF9) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||   // Comb Diacriticals Suppl
        (cp >= 0x200B && cp <= 0x200F) ||   // Zero-width, LRM, RLM
        (cp >= 0x2028 && cp <= 0x202E) ||   // Line separators, directional
        (cp >= 0x2060 && cp <= 0x206F) ||   // Word joiner, invisibles
        (cp >= 0x20D0 && cp <= 0x20F0) ||   // Comb Marks for Symbols
        (cp >= 0x2CEF && cp <= 0x2CF1) ||   // Coptic
        (cp >= 0x2D7F && cp <= 0x2D7F) ||   // Tifinagh
        (cp >= 0x2DE0 && cp <= 0x2DFF) ||   // Cyrillic Ext-A
        (cp >= 0xA66F && cp <= 0xA672) ||   // Cyrillic Ext-B
        (cp >= 0xA674 && cp <= 0xA67D) ||
        (cp >= 0xA69E && cp <= 0xA69F) ||
        (cp >= 0xA6F0 && cp <= 0xA6F1) ||
        (cp >= 0xA802 && cp <= 0xA802) ||   // Syloti Nagri
        (cp >= 0xA806 && cp <= 0xA806) ||
        (cp >= 0xA80B && cp <= 0xA80B) ||
        (cp >= 0xA823 && cp <= 0xA827) ||
        (cp >= 0xA880 && cp <= 0xA881) ||   // Saurashtra
        (cp >= 0xA8B4 && cp <= 0xA8C5) ||
        (cp >= 0xA8E0 && cp <= 0xA8F1) ||   // Devanagari Extended
        (cp >= 0xA926 && cp <= 0xA92D) ||   // Kayah Li
        (cp >= 0xA947 && cp <= 0xA953) ||   // Rejang
        (cp >= 0xA980 && cp <= 0xA983) ||   // Javanese
        (cp >= 0xA9B3 && cp <= 0xA9C0) ||
        (cp >= 0xA9E5 && cp <= 0xA9E5) ||   // Myanmar
        (cp >= 0xAA29 && cp <= 0xAA36) ||   // Cham
        (cp >= 0xAA43 && cp <= 0xAA43) ||
        (cp >= 0xAA4C && cp <= 0xAA4D) ||
        (cp >= 0xAA7B && cp <= 0xAA7D) ||   // Myanmar
        (cp >= 0xAAB0 && cp <= 0xAAB0) ||   // Tai Viet
        (cp >= 0xAAB2 && cp <= 0xAAB4) ||
        (cp >= 0xAAB7 && cp <= 0xAAB8) ||
        (cp >= 0xAABE && cp <= 0xAABF) ||
        (cp >= 0xAAC1 && cp <= 0xAAC1) ||
        (cp >= 0xAAEB && cp <= 0xAAEF) ||   // Meetei Mayek
        (cp >= 0xAAF5 && cp <= 0xAAF6) ||
        (cp >= 0xABE3 && cp <= 0xABED) ||   // Meetei Mayek Ext
        (cp >= 0xFE00 && cp <= 0xFE0F) ||   // Variation Selectors
        (cp >= 0xFE20 && cp <= 0xFE2F) ||   // Combining Half Marks
        (cp >= 0xFEFF && cp <= 0xFEFF))     // BOM / ZWNBSP
        return 0;

    // East Asian Wide / Fullwidth / Emoji ranges (simplified but covers
    // CJK, Hangul, and common emoji)
    if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
        (cp >= 0x2329 && cp <= 0x232A) ||   // Misc Technical
        (cp >= 0x2E80 && cp <= 0xA4CF) ||   // CJK Rad Suppl … Yijing
        (cp >= 0xA960 && cp <= 0xA97C) ||   // Hangul Jamo Extended-A
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul Syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compat Ideographs
        (cp >= 0xFE10 && cp <= 0xFE19) ||   // Vertical Forms
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK Compat Forms
        (cp >= 0xFF01 && cp <= 0xFF60) ||   // Fullwidth Forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // Fullwidth Signs
        (cp >= 0x1F300 && cp <= 0x1F64F) || // Emoticons
        (cp >= 0x1F900 && cp <= 0x1F9FF) || // Supplemental Symbols
        (cp >= 0x20000 && cp <= 0x2FFFD) || // CJK Ext B+
        (cp >= 0x30000 && cp <= 0x3FFFD))   // CJK Ext G+
        return 2;

    return 1;
}

auto displayWidth(std::string_view s) -> std::size_t {
    std::size_t w = 0;
    const auto* it = s.begin();
    while (it != s.end()) {
        w += static_cast<std::size_t>(cpWidth(decodeUtf8(it, s.end())));
    }
    return w;
}

auto isTty(int fd) -> bool {
    return ::isatty(fd) != 0;
}

} // anonymous namespace

// =============================================================================
// Pipeline operators
// =============================================================================

Table operator|(Header h, DataRow r) {
    Table t;
    t.header(std::move(h.cols));
    t.data(std::move(r.cols));
    return t;
}

Table operator|(Header h, SepLine s) {
    Table t;
    t.header(std::move(h.cols));
    t.sep(s.c);
    return t;
}

Table operator|(Table&& t, DataRow r) {
    t.data(std::move(r.cols));
    return t;
}

Table operator|(Table&& t, SepLine s) {
    t.sep(s.c);
    return t;
}

Table operator|(const Table& t, DataRow r) {
    Table copy = t;
    copy.data(std::move(r.cols));
    return copy;
}

Table operator|(const Table& t, SepLine s) {
    Table copy = t;
    copy.sep(s.c);
    return copy;
}

// =============================================================================
// Table: imperative API
// =============================================================================

auto Table::header(std::vector<std::string> cols) -> Table& {
    pushHeader(std::move(cols));
    return *this;
}

auto Table::data(std::vector<std::string> cols) -> Table& {
    pushData(std::move(cols));
    return *this;
}

auto Table::sep(char32_t c) -> Table& {
    pushSep(c);
    return *this;
}

// =============================================================================
// Table: internal builders
// =============================================================================

void Table::pushHeader(std::vector<std::string> cols) {
    assert(rows_.empty()); // header must be first
    if (!bordersExplicit_)
        borders_ = isTty(STDOUT_FILENO);
    maxCols_ = cols.size();
    rows_.push_back({std::move(cols), RowKind::kData, U'\0'});
}

void Table::pushData(std::vector<std::string> cols) {
    if (!bordersExplicit_ && rows_.empty())
        borders_ = isTty(STDOUT_FILENO);
    if (cols.size() > maxCols_)
        maxCols_ = cols.size();
    rows_.push_back({std::move(cols), RowKind::kData, U'\0'});
}

void Table::pushSep(char32_t c) {
    if (!bordersExplicit_ && rows_.empty())
        borders_ = isTty(STDOUT_FILENO);
    rows_.push_back({{}, RowKind::kSep, c});
}

// =============================================================================
// Table: rendering
// =============================================================================

void Table::render() const {
    render(std::cout);
}

void Table::render(std::ostream& out) const {
    if (rows_.empty())
        return;

    auto widths = colWidths();
    auto total = totalWidth(widths, maxCols_);

    if (borders_) {
        printBorder(out, total, U'=');
        printData(out, rows_[0].cols, widths);
        printBorder(out, total, U'=');
    } else if (!rows_[0].cols.empty()) {
        printData(out, rows_[0].cols, widths);
    }

    for (std::size_t i = 1; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        if (r.kind == RowKind::kSep) {
            if (borders_)
                printBorder(out, total, r.sepChar);
        } else {
            printData(out, r.cols, widths);
        }
    }

    if (borders_)
        printBorder(out, total, U'=');
}

auto Table::str() const -> std::string {
    std::ostringstream oss;
    render(oss);
    return oss.str();
}

// =============================================================================
// Table: column / rendering helpers
// =============================================================================

auto Table::colWidths() const -> std::vector<std::size_t> {
    std::vector<std::size_t> w(maxCols_, 0);
    for (const auto& r : rows_) {
        if (r.kind == RowKind::kSep)
            continue;
        for (std::size_t i = 0; i < r.cols.size(); ++i) {
            auto cw = displayWidth(r.cols[i]);
            if (cw > w[i])
                w[i] = cw;
        }
    }
    return w;
}

auto Table::totalWidth(const std::vector<std::size_t>& widths, std::size_t maxCols) -> std::size_t {
    std::size_t n = maxCols * 3 + 1;
    for (auto w : widths)
        n += w;
    return n;
}

void Table::printBorder(std::ostream& out, std::size_t total, char32_t ch) {
    auto w = static_cast<std::size_t>(cpWidth(ch));
    if (w == 0 || total < 2) return; // guard: control char or empty table
    auto utf8 = toUtf8(ch);
    auto count = (total - 2) / w;
    std::string line = "+";
    line.reserve(2 + count * utf8.size());
    for (std::size_t i = 0; i < count; ++i)
        line += utf8;
    line += '+';
    out << line << '\n';
}

void Table::printData(std::ostream& out,
                      const std::vector<std::string>& cols,
                      const std::vector<std::size_t>& widths) const {
    if (borders_) {
        out << "| ";
    }

    for (std::size_t i = 0; i < maxCols_; ++i) {
        std::string val = i < cols.size() ? cols[i] : std::string{};
        out << padRight(std::move(val), widths[i], ' ');

        if (borders_) {
            out << " |";
        }
        if (i + 1 < maxCols_) {
            out << ' ';
        }
    }
    out << '\n';
}

auto Table::padRight(std::string s, std::size_t width, char pad) -> std::string {
    auto cur = displayWidth(s);
    if (cur >= width)
        return s;
    s.reserve(s.size() + (width - cur));
    s.append(width - cur, pad);
    return s;
}

} // namespace pl::pretty
