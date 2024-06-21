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
