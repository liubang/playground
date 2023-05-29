//=====================================================================
//
// bits.h -
//
// Created by liubang on 2023/05/29 22:44
// Last Modified: 2023/05/29 22:44
//
//=====================================================================

#include <bit>
#include <cstdint>
#include <type_traits>

namespace playground::cpp::tools {

namespace detail {

constexpr auto kIsLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
constexpr auto kIsBigEndian = !kIsLittleEndian;

template <std::size_t Size>
struct uint_types_by_size;

#define BIT_GEN(sz, fn)                                                     \
  static inline uint##sz##_t byteswap_gen(uint##sz##_t v) { return fn(v); } \
  template <>                                                               \
  struct uint_types_by_size<sz / 8> {                                       \
    using type = uint##sz##_t;                                              \
  };

BIT_GEN(8, uint8_t)
BIT_GEN(64, __builtin_bswap64)
BIT_GEN(32, __builtin_bswap32)
BIT_GEN(16, __builtin_bswap16)

#undef BIT_GEN

template <class T>
struct EndianInt {
  static_assert((std::is_integral<T>::value && !std::is_same<T, bool>::value) ||
                    std::is_floating_point<T>::value,
                "template type parameter must be non-bool integral or floating point");

  static T swap(T x) {
    constexpr auto s = sizeof(T);
    using B = typename uint_types_by_size<s>::type;
    return std::bit_cast<T>(byteswap_gen(std::bit_cast<B>(x)));
  }

  static T big(T x) { return kIsLittleEndian ? EndianInt::swap(x) : x; }
  static T little(T x) { return kIsBigEndian ? EndianInt::swap(x) : x; }
};

}  // namespace detail

class Endian {
public:
  enum class Order : uint8_t {
    LITTLE,
    BIG,
  };

  static constexpr Order order = detail::kIsLittleEndian ? Order::LITTLE : Order::BIG;

  template <class T>
  static T swap(T x) {
    return detail::EndianInt<T>::swap(x);
  }

  template <class T>
  static T big(T x) {
    return detail::EndianInt<T>::big(x);
  }

  template <class T>
  static T little(T x) {
    return detail::EndianInt<T>::little(x);
  }
};

}  // namespace playground::cpp::tools
