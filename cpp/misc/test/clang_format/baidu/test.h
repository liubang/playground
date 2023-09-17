#pragma once

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

// extern
#ifdef __cplusplus
extern "C" {
#endif

int add(int a, int b) {
    return a + b;
}

#ifdef __cplusplus
}
#endif

// macro
#ifndef TEST_DEFINE
#define TEST_DEFINE
#endif

#define __DUMMY_MAX(a, b) \
    do {                  \
        if ((a) > (b)) {  \
            return (a);   \
        } else {          \
            return (b);   \
        }                 \
    } while (0)

namespace pl::test {

// test extern
extern const std::string TEST_EXTERN;

// enum
enum class FOO_ENUM : uint8_t {
    ENUM1,
    ENUM2,
    ENUM3,
    ENUM4,
    ENUM5,
};

// struct
struct Node {
    std::string name;
    FOO_ENUM type;
};

// raw string
std::string raw_string = R"(
    this is raw string 
    this is raw string 
    this is raw string 
    this is raw string
)";

// long string
std::string long_string = "this is long string, this is long string, this is "
                          "long string, this is long "
                          "string, this is long string, this is long string, "
                          "this is long string, this is "
                          "long string, this is long string";

// lambda
auto fn = [](int a, int b) {
    return a + b;
};

// class
class Foo {
public:
    Foo(std::string attr1,
        std::string attr2,
        std::string attr3,
        std::string attr4,
        const std::unordered_map<std::string, std::string> &attr5,
        uint64_t attr6,
        const std::shared_ptr<std::string> &attr7)
        : attr1_(std::move(attr1)),
          attr2_(std::move(attr2)),
          attr3_(std::move(attr3)),
          attr4_(std::move(attr4)),
          attr5_(attr5),
          attr6_(attr6),
          attr7_(attr7) {}

    virtual ~Foo() = default;

    uint64_t attr6() const { return attr6_; }

    void test_switch(FOO_ENUM e) {
        switch (e) {
        case FOO_ENUM::ENUM1: {
            std::cout << "enum1" << '\n';
            break;
        }
        case FOO_ENUM::ENUM2:
            break;
        case FOO_ENUM::ENUM3:
            break;
        case FOO_ENUM::ENUM4:
            break;
        case FOO_ENUM::ENUM5:
            break;
        default:
            std::cout << "invalid type" << '\n';
        }
    }

    void test_pointer(const char *s) { ::printf("s = %s\n", s); }

    void test_cf(const std::string &s) { std::cout << s << '\n'; }

private:
    std::string attr1_;
    std::string attr2_;
    std::string attr3_;
    std::string attr4_;
    std::unordered_map<std::string, std::string> attr5_;
    uint64_t attr6_;
    std::shared_ptr<std::string> attr7_;
};

namespace detail {

constexpr auto kIsLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
constexpr auto kIsBigEndian    = !kIsLittleEndian;

static inline uint8_t byteswap_gen(uint8_t v) {
    return uint8_t(v);
}
static inline uint16_t byteswap_gen(uint16_t v) {
    return __builtin_bswap16(v);
}
static inline uint32_t byteswap_gen(uint32_t v) {
    return __builtin_bswap32(v);
}
static inline uint64_t byteswap_gen(uint64_t v) {
    return __builtin_bswap64(v);
}

template <std::size_t Size> struct uint_types_by_size;

template <> struct uint_types_by_size<1> {
    using type = uint8_t;
};

template <> struct uint_types_by_size<2> {
    using type = uint16_t;
};

template <> struct uint_types_by_size<4> {
    using type = uint32_t;
};

template <> struct uint_types_by_size<8> {
    using type = uint64_t;
};

template <class To, class From>
std::enable_if_t<sizeof(To) == sizeof(From) && std::is_trivially_copyable_v<From> &&
                     std::is_trivially_copyable_v<To>,
                 To>
// constexpr support needs compiler magic
bit_cast(const From &src) noexcept {
    static_assert(std::is_trivially_constructible_v<To>,
                  "This implementation additionally requires "
                  "destination type to be trivially constructible");

    To dst;
    std::memcpy(&dst, &src, sizeof(To));
    return dst;
}

template <class T> struct EndianInt {
    static_assert((std::is_integral<T>::value && !std::is_same<T, bool>::value) ||
                      std::is_floating_point<T>::value,
                  "template type parameter must be non-bool integral or floating point");

    static T swap(T x) {
        constexpr auto s = sizeof(T);
        using B          = typename uint_types_by_size<s>::type;
        return bit_cast<T>(byteswap_gen(bit_cast<B>(x)));
    }

    static T big(T x) { return kIsLittleEndian ? EndianInt::swap(x) : x; }
    static T little(T x) { return kIsBigEndian ? EndianInt::swap(x) : x; }
};

} // namespace detail

class Endian {
public:
    enum class Order : uint8_t {
        LITTLE,
        BIG,
    };

    static constexpr Order order = detail::kIsLittleEndian ? Order::LITTLE : Order::BIG;

    template <class T> static T swap(T x) { return detail::EndianInt<T>::swap(x); }

    template <class T> static T big(T x) { return detail::EndianInt<T>::big(x); }

    template <class T> static T little(T x) { return detail::EndianInt<T>::little(x); }
};

} // namespace pl::test
