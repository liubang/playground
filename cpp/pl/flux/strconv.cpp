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

#include "strconv.h"

#include <iomanip>
#include <sstream>

namespace pl {

bool StrConv::to_byte(unsigned char c, uint8_t* b) {
    if (c >= '0' && c <= '9') {
        *b = c - '0';
    } else if (c >= 'a' && c <= 'f') {
        *b = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        *b = c - 'A' + 10;
    } else {
        return false;
    }
    return true;
}

absl::StatusOr<std::string> StrConv::push_hex_byte(const std::string& lit,
                                                   size_t& start,
                                                   std::string* s) {
    if (start == lit.length() - 1) {
        return absl::InvalidArgumentError("\\x followed by 0 char, must be 2");
    }
    auto ch1 = lit[++start];
    if (start == lit.length() - 1) {
        return absl::InvalidArgumentError("\\x followed by 1 char, must be 2");
    }
    auto ch2 = lit[++start];
    uint8_t b1, b2;
    if (!to_byte(ch1, &b1) || !to_byte(ch2, &b2)) {
        return absl::InvalidArgumentError("invalid byte value");
    }
    uint8_t b = (b1 << 4) | b2;
    s->push_back(static_cast<char>(b));

    return absl::OkStatus();
}

absl::StatusOr<std::string> StrConv::parse_text(const std::string& lit) {
    std::string s;
    for (size_t i = 0; i < lit.length(); ++i) {
        char c = lit[i];
        if (c != '\\') {
            s.push_back(c);
            continue;
        }
        ++i;
        if (i == lit.length()) {
            return absl::InvalidArgumentError("invalid escape sequence");
        }
        char n = lit[i];
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
        case 'x':
        {
            auto ret = push_hex_byte(lit, i, &s);
            if (!ret.ok()) {
                return ret;
            }
            break;
        }
        default:
            return absl::InvalidArgumentError("invalid escape sequence " + std::string(n, 1));
        }
    }
    return s;
}

// TODO(liubang): implementd
absl::StatusOr<std::string> StrConv::parse_string(const std::string& lit) {
    if (lit.length() < 2 || !lit.starts_with('"') || !lit.ends_with('"')) {
        return absl::InvalidArgumentError("invalid string literal");
    }
    return parse_text(lit.substr(1, lit.length() - 1));
}

absl::StatusOr<std::string> StrConv::parse_regex(const std::string& lit) {
    if (lit.length() < 3) {
        return absl::InvalidArgumentError("regexp must be at least 3 characters");
    }
    if (!lit.starts_with('/')) {
        return absl::InvalidArgumentError("regexp must be start with a slash");
    }
    if (!lit.ends_with('/')) {
        return absl::InvalidArgumentError("regexp must be end with a slash");
    }

    auto expr = lit.substr(1, lit.length() - 2);
    std::string unescaped;
    for (size_t i = 0; i < expr.length(); ++i) {
        char c = expr[i];
        if (c == '\\') {
            ++i;
            if (i == expr.length()) {
                return absl::InvalidArgumentError("unterminated regex sequence");
            }
            char cc = expr[i];
            if (cc == '/') {
                unescaped.push_back('/');
            } else if (cc == 'x') {
                auto ret = push_hex_byte(expr, i, &unescaped);
                if (!ret.ok()) {
                    return ret;
                }
            } else if (cc == c) {
                unescaped.push_back('\\');
                unescaped.push_back(c);
            }
        } else {
            unescaped.push_back(c);
        }
    }
    return unescaped;
}

absl::StatusOr<std::tm> StrConv::parse_time(const std::string& lit) {
    std::istringstream s(lit);
    std::tm t = {};
    if (lit.find_first_of('T') == std::string::npos) {
        constexpr static char datefmt[] = "%Y-%m-%d";
        s >> std::get_time(&t, datefmt);
    } else {
        constexpr static char rfc3339[] = "%Y-%m-%dT%H:%M:%SZ";
        // parse rfc3339 time format
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
    for (size_t i = 0; i < lit.length();) {
        auto magnitude = parse_magnitude(lit, i);
        if (!magnitude.ok()) {
            return magnitude.status();
        }
        auto unit = parse_unit(lit, i);
        if (!unit.ok()) {
            return unit.status();
        }
        values.emplace_back(std::make_shared<Duration>(magnitude.value(), unit.value()));
    }
    return values;
}

absl::StatusOr<int64_t> StrConv::parse_magnitude(const std::string& str, size_t& i) {
    size_t l = str.size();
    std::string s;
    while (i < l) {
        unsigned char c = str[i];
        if (std::isdigit(c) == 0) {
            break;
        }
        s.push_back(c);
        ++i;
    }

    if (s.empty()) {
        return absl::InvalidArgumentError("parsing empty magnitude");
    }

    try {
        int64_t ret = std::stol(s);
        return ret;
    } catch (const std::invalid_argument& ia) {
        return absl::InvalidArgumentError(ia.what());
    } catch (const std::out_of_range& oor) {
        return absl::InvalidArgumentError(oor.what());
    }
}

absl::StatusOr<std::string> StrConv::parse_unit(const std::string& chars, size_t& i) {
    size_t l = chars.size();
    std::string u;
    while (i < l) {
        unsigned char c = chars[i];
        if (std::isalpha(c) == 0 && c != DURATION_UNIT_US[0]) {
            break;
        }
        u.push_back(c);
        if (c == DURATION_UNIT_US[0]) {
            u.push_back(chars[++i]);
        }
        ++i;
    }
    if (u.empty()) {
        return absl::InvalidArgumentError("parsing empty unit");
    }
    if (u == "µs") {
        u = "us";
    }
    return u;
}

} // namespace pl
