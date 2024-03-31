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

#include <fstream>
#include <iomanip>
#include <iostream>
#include <source_location>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace pl {

struct logger {
public:
private:
    std::ostringstream oss_;

    enum { DEBUG = 0, TRACE = 1, INFO = 2, ERROR = 3 } LEVEL;

    const char* line_;
    const std::source_location& loc_;

    static void quotes(std::ostream& os, std::string_view sv, char quote) {
        os << quote;
        for (char c : sv) {
            switch (c) {
            case '\n':
                os << "\\n";
                break;
            case '\r':
                os << "\\r";
                break;
            case '\t':
                os << "\\t";
                break;
            case '\\':
                os << "\\\\";
                break;
            case '\0':
                os << "\\0";
                break;
            default:
                if ((c >= 0 && c < 0x20) || c == 0x7F) {
                    auto f = os.flags();
                    os << "\\x" << std::hex << std::setfill('0') << std::setw(2)
                       << static_cast<int>(c);
                    os.flags(f);
                } else {
                    if (c == quote) {
                        os << '\\';
                    }
                    os << c;
                }
            }
        }
        os << quote;
    }
};

} // namespace pl
