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

#include <cstddef>

namespace pl {

template <typename... Ts> struct TypeList {
    struct IsTypeList {};
    using type = TypeList;
    constexpr static std::size_t size = sizeof...(Ts);
    template <typename... T> using append = TypeList<Ts..., T...>;
    template <typename... T> using prepend = TypeList<T..., Ts...>;
    template <template <typename...> typename T> using to = T<Ts...>;
};

template <typename TypeList>
concept TL = requires {
    typename TypeList::IsTypeList; // 约束
    typename TypeList::type;       // 返回值
};

} // namespace pl
