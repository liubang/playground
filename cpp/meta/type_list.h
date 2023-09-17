//=====================================================================
//
// type_list.h -
//
// Created by liubang on 2023/06/11 20:20
// Last Modified: 2023/06/11 20:20
//
//=====================================================================
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
