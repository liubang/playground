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

#include <string>
#include <vector>

#include "ast.h"

#include "absl/status/statusor.h"

namespace pl {

constexpr unsigned char DURATION_UNIT_US[] = "µ";

class StrConv {
public:
    static bool to_byte(unsigned char c, uint8_t* b);

    static absl::StatusOr<std::string> push_hex_byte(const std::string& lit,
                                                     size_t& start,
                                                     std::string* s);

    static absl::StatusOr<std::string> parse_text(const std::string& lit);

    static absl::StatusOr<std::string> parse_string(const std::string& lit);

    static absl::StatusOr<std::string> parse_regex(const std::string& lit);

    // parse time from string
    static absl::StatusOr<std::tm> parse_time(const std::string& lit);

    static absl::StatusOr<std::vector<std::shared_ptr<Duration>>> parse_duration(
        const std::string& lit);

    static absl::StatusOr<int64_t> parse_magnitude(const std::string& str, size_t& i);

    static absl::StatusOr<std::string> parse_unit(const std::string& chars, size_t& i);
};

} // namespace pl
