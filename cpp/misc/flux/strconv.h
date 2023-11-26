//=====================================================================
//
// strconv.h -
//
// Created by liubang on 2023/11/04 17:26
// Last Modified: 2023/11/04 17:26
//
//=====================================================================

#include <string>
#include <vector>

#include "ast.h"

#include "absl/status/statusor.h"

namespace pl {

constexpr unsigned char DURATION_UNIT_US[] = "µ";

class StrConv {
public:
    static bool to_byte(char c, uint8_t* b);

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
