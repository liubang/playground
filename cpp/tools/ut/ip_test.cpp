//=====================================================================
//
// ip_test.cpp -
//
// Created by liubang on 2023/05/21 00:55
// Last Modified: 2023/05/21 00:55
//
//=====================================================================

#include "cpp/tools/ip.h"

#include <gtest/gtest.h>

#include <iostream>

TEST(tools, ip) {
  auto ip = playground::cpp::tools::getLocalIp();
  std::cout << ip.value() << std::endl;
  EXPECT_FALSE(ip->empty());
}
