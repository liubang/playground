//=====================================================================
//
// val.h -
//
// Created by liubang on 2021/01/07 18:07
// Last Modified: 2021/01/07 18:07
//
//=====================================================================
#pragma once

#include <list>
#include <set>
#include <string>
#include <unordered_map>

namespace highkyck {

enum class Type
{
  INT_VAL = 1,
  STRING_VAL = 2,
  MAP_VAL = 3,
  LIST_VAL = 4,
  SET_VAL = 5
};

struct Val
{
  Type t;
  union
  {
    uint64_t int_val;
    std::string string_val;
    std::unordered_map<std::string, std::string> map_val;
    std::list<std::string> list_val;
    std::set<std::string> set_val;
  } v;
};
}  // namespace highkyck
