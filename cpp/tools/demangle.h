//=====================================================================
//
// demangle.h -
//
// Created by liubang on 2023/10/06 23:09
// Last Modified: 2023/10/06 23:09
//
//=====================================================================

#pragma once

#include <string>
#include <type_traits>
#include <typeinfo>
#if (defined(__GNUC__) || defined(__clang__)) && __has_include(<cxxabi.h>)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace pl {
namespace detail {

static std::string demangle(const char* name) {
#if (defined(__GNUC__) || defined(__clang__)) && __has_include(<cxxabi.h>)
    int status;
    char* p = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string s = p != nullptr ? p : name;
    std::free(p);
#else
    std::string s = name;
#endif
    return s;
}

static std::string demangle(std::type_info const& type) { return demangle(type.name()); }

template <typename T> static std::string demangle() {
    std::string s{demangle(typeid(std::remove_cv_t<std::remove_reference_t<T>>))};
    if (std::is_const_v<std::remove_reference_t<T>>) {
        s += " const";
    }
    if (std::is_volatile_v<std::remove_reference_t<T>>) {
        s += " volatile";
    }
    if (std::is_rvalue_reference_v<T>) {
        s += " &&";
    }
    return s;
}

} // namespace detail

using detail::demangle;

} // namespace pl
