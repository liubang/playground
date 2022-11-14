#pragma once

namespace test {
namespace cpp14 {

template <char c>
constexpr bool is_digit = (c >= '0' && c <= '9');

}
}  // namespace test
