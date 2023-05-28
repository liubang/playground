//=====================================================================
//
// json_test.cpp -
//
// Created by liubang on 2023/05/28 21:07
// Last Modified: 2023/05/28 21:07
//
//=====================================================================

#include "cpp/misc/json/json.h"

#include <gtest/gtest.h>

TEST(json, parse) {
  std::string str1 = "123";
  playground::cpp::misc::json::parse(str1);
}
