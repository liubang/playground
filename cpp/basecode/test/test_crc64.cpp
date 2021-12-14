#include <gtest/gtest.h>

#include <iostream>

#include "basecode/crc64.h"

TEST(crc64, crc64) {
  auto ret = basecode::crc64(0, static_cast<const char*>("123456789"), 9);
  std::cout << ret << std::endl;
}
