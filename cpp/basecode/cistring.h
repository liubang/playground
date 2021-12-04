#pragma once

#include <ostream>
#include <string>

#include <iostream>

namespace basecode {

struct cichar_traits : public std::char_traits<char> {
  static constexpr char to_upper_case(const char c) {
    return ('A' <= c && c <= 'Z') ? c + 32 : c;
  }

  static constexpr bool eq(const char a, const char b) {
    return to_upper_case(a) == to_upper_case(b);
  }

  static constexpr bool lt(const char a, const char b) {
    return to_upper_case(a) < to_upper_case(b);
  }

  static constexpr int compare(const char *str1, const char *str2,
                               std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      if (lt(str1[i], str2[i])) {
        return -1;
      } else if (lt(str2[i], str1[i])) {
        return 1;
      }
    }
    return 0;
  }

  static constexpr const char *find(const char *s, std::size_t n,
                                    const char &a) {
    for (std::size_t i = 0; i < n; ++i) {
      if (eq(s[i], a)) {
        return s + i;
      }
    }
    return nullptr;
  }

  static constexpr bool eq_int_type(int_type c1, int_type c2) {
    return eq(to_char_type(c1), to_char_type(c2));
  }
};

using cistring = std::basic_string<char, cichar_traits>;

inline std::ostream &operator<<(std::ostream &os, const cistring &str) {
  os << str.c_str();
  return os;
}

} // namespace basecode
