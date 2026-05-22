// Copyright (c) 2023 The Authors. All rights reserved.
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
// Created: 2023/11/26 22:05

#include "strconv.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace pl::flux {

namespace {

bool starts_with_char(const std::string& text, char ch) {
    return !text.empty() && text.front() == ch;
}

bool ends_with_char(const std::string& text, char ch) {
    return !text.empty() && text.back() == ch;
}

} // namespace

bool StrConv::to_byte(unsigned char c, uint8_t* b) {
    if (c >= '0' && c <= '9') {
        *b = static_cast<uint8_t>(c - static_cast<unsigned char>('0'));
    } else if (c >= 'a' && c <= 'f') {
        *b = static_cast<uint8_t>(c - static_cast<unsigned char>('a') + 10);
    } else if (c >= 'A' && c <= 'F') {
        *b = static_cast<uint8_t>(c - static_cast<unsigned char>('A') + 10);
    } else {
        return false;
    }
    return true;
}

absl::Status StrConv::push_hex_byte(const std::string& lit, size_t& start, std::string* s) {
    if (start + 2 >= lit.length()) {
        if (start + 1 >= lit.length()) {
            return absl::InvalidArgumentError("\\x followed by 0 char, must be 2");
        }
        return absl::InvalidArgumentError("\\x followed by 1 char, must be 2");
    }

    const auto ch1 = static_cast<unsigned char>(lit[++start]);
    const auto ch2 = static_cast<unsigned char>(lit[++start]);
    uint8_t b1 = 0;
    uint8_t b2 = 0;
    if (!to_byte(ch1, &b1) || !to_byte(ch2, &b2)) {
        return absl::InvalidArgumentError("invalid byte value");
    }
    const auto byte = static_cast<uint8_t>((b1 << 4) | b2);
    s->push_back(static_cast<char>(byte));
    return absl::OkStatus();
}

absl::StatusOr<std::string> StrConv::parse_text(const std::string& lit) {
    std::string s;
    s.reserve(lit.size());
    for (size_t i = 0; i < lit.length(); ++i) {
        const char c = lit[i];
        if (c != '\\') {
            s.push_back(c);
            continue;
        }
        ++i;
        if (i == lit.length()) {
            return absl::InvalidArgumentError("invalid escape sequence");
        }
        const char n = lit[i];
        switch (n) {
            case 'n':
                s.push_back('\n');
                break;
            case 'r':
                s.push_back('\r');
                break;
            case 't':
                s.push_back('\t');
                break;
            case '\\':
                s.push_back('\\');
                break;
            case '"':
                s.push_back('"');
                break;
            case '$':
                s.push_back('$');
                break;
            case 'x': {
                auto ret = push_hex_byte(lit, i, &s);
                if (!ret.ok()) {
                    return ret;
                }
                break;
            }
            default:
                return absl::InvalidArgumentError("invalid escape sequence " + std::string(1, n));
        }
    }
    return s;
}

absl::StatusOr<std::string> StrConv::parse_string(const std::string& lit) {
    if (lit.length() < 2 || !starts_with_char(lit, '"') || !ends_with_char(lit, '"')) {
        return absl::InvalidArgumentError("invalid string literal");
    }
    return parse_text(lit.substr(1, lit.length() - 2));
}

absl::StatusOr<std::string> StrConv::parse_regex(const std::string& lit) {
    if (lit.length() < 3) {
        return absl::InvalidArgumentError("regexp must be at least 3 characters");
    }
    if (!starts_with_char(lit, '/')) {
        return absl::InvalidArgumentError("regexp must be start with a slash");
    }
    if (!ends_with_char(lit, '/')) {
        return absl::InvalidArgumentError("regexp must be end with a slash");
    }

    const auto expr = lit.substr(1, lit.length() - 2);
    std::string unescaped;
    unescaped.reserve(expr.size());
    for (size_t i = 0; i < expr.length(); ++i) {
        const char c = expr[i];
        if (c != '\\') {
            unescaped.push_back(c);
            continue;
        }

        ++i;
        if (i == expr.length()) {
            return absl::InvalidArgumentError("unterminated regex sequence");
        }
        const char escaped = expr[i];
        if (escaped == '/') {
            unescaped.push_back('/');
        } else if (escaped == 'x') {
            auto ret = push_hex_byte(expr, i, &unescaped);
            if (!ret.ok()) {
                return ret;
            }
        } else {
            unescaped.push_back('\\');
            unescaped.push_back(escaped);
        }
    }
    return unescaped;
}

absl::StatusOr<std::tm> StrConv::parse_time(const std::string& lit) {
    std::istringstream s(lit);
    std::tm t = {};
    if (lit.find('T') == std::string::npos) {
        constexpr char datefmt[] = "%Y-%m-%d";
        s >> std::get_time(&t, datefmt);
    } else {
        constexpr char rfc3339[] = "%Y-%m-%dT%H:%M:%SZ";
        s >> std::get_time(&t, rfc3339);
    }
    if (s.fail()) {
        return absl::InvalidArgumentError("fail to parse time " + lit);
    }
    return t;
}

absl::StatusOr<std::vector<std::shared_ptr<Duration>>> StrConv::parse_duration(
    const std::string& lit) {
    std::vector<std::shared_ptr<Duration>> values;
    values.reserve(2);
    for (size_t i = 0; i < lit.length();) {
        auto magnitude = parse_magnitude(lit, i);
        if (!magnitude.ok()) {
            return magnitude.status();
        }
        auto unit = parse_unit(lit, i);
        if (!unit.ok()) {
            return unit.status();
        }
        values.emplace_back(std::make_shared<Duration>(*magnitude, *unit));
    }
    return values;
}

absl::StatusOr<int64_t> StrConv::parse_magnitude(const std::string& str, size_t& i) {
    const size_t length = str.size();
    if (i >= length || std::isdigit(static_cast<unsigned char>(str[i])) == 0) {
        return absl::InvalidArgumentError("parsing empty magnitude");
    }

    int64_t value = 0;
    while (i < length) {
        const auto digit = static_cast<unsigned char>(str[i]);
        if (std::isdigit(digit) == 0) {
            break;
        }
        value = (value * 10) + static_cast<int64_t>(digit - static_cast<unsigned char>('0'));
        ++i;
    }
    return value;
}

absl::StatusOr<std::string> StrConv::parse_unit(const std::string& chars, size_t& i) {
    const size_t length = chars.size();
    std::string unit;
    unit.reserve(2);
    while (i < length) {
        const auto c = static_cast<unsigned char>(chars[i]);
        if (std::isalpha(c) == 0 && c != DURATION_UNIT_US[0]) {
            break;
        }
        unit.push_back(static_cast<char>(c));
        if (c == DURATION_UNIT_US[0]) {
            ++i;
            if (i >= length) {
                return absl::InvalidArgumentError("unterminated microsecond unit");
            }
            unit.push_back(chars[i]);
        }
        ++i;
    }
    if (unit.empty()) {
        return absl::InvalidArgumentError("parsing empty unit");
    }
    if (unit == "µ") {
        return absl::InvalidArgumentError("unterminated microsecond unit");
    }
    if (unit == "µs") {
        unit = DURATION_UNIT_US_ASCII;
    }
    return unit;
}

} // namespace pl::flux
