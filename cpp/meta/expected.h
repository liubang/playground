//=====================================================================
//
// expected.h -
//
// Created by liubang on 2023/11/24 17:45
// Last Modified: 2023/11/24 17:45
//
//=====================================================================

#include <algorithm>
#include <type_traits>

namespace pl {

template <class Error> class Unexpected;

template <class Value, class Error> class Expected;

template <class Expected>
using ExpectedValueType = typename std::remove_reference<Expected>::type::value_type;

template <class Expected>
using ExpectedErrorType = typename std::remove_reference<Expected>::type::error_type;

namespace expectd_detail {

enum class Which : unsigned char { eEmpty, eValue, eError };

template <class Value, class Error> class ExpectedStorage {
    using value_type = Value;
    using error_type = Error;
    union {
        Value value_;
        Error error_;
        char ch_;
    };
    Which which_;
};

}; // namespace expectd_detail

template <class Value, class Error> class Expected final {};

/**
 * Unexpected: a helper type used to disambiguate the construction of Expected objects in the error
 * state.
 */
template <class Error> class Unexpected final {
    template <class E> friend class Unexpected;
    template <class V, class E> friend class Expected;

public:
    Unexpected() = default;
    Unexpected(const Unexpected&) = default;
    Unexpected(Unexpected&&) noexcept = default;
    Unexpected& operator=(const Unexpected&) = default;
    Unexpected& operator=(Unexpected&&) noexcept = default;

    constexpr Unexpected(const Error& err) : error_(err) {}
    constexpr Unexpected(Error&& err) : error_(std::move(err)) {}

    Error& error() & { return error_; }
    const Error& error() const& { return error_; }
    Error&& error() && { return std::move(error_); }

private:
    Error error_;
};

} // namespace pl
