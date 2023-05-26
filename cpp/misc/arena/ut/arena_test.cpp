//=====================================================================
//
// arena_test.cpp -
//
// Created by liubang on 2023/05/21 01:40
// Last Modified: 2023/05/21 01:40
//
//=====================================================================
#include "cpp/misc/arena/arena.h"

#include <gtest/gtest.h>

TEST(arena, allocate) {
  constexpr std::size_t ptr_char_size = sizeof(char*);
  playground::cpp::misc::arena::Arena arena;

  arena.allocate(1024);
  int usage = 4096 + ptr_char_size;
  EXPECT_EQ(arena.memory_usage(), usage);

  arena.allocate(2048);
  EXPECT_EQ(arena.memory_usage(), usage);

  arena.allocate(2048);
  usage += 2048 + ptr_char_size;
  EXPECT_EQ(arena.memory_usage(), usage);

  arena.allocate(1);
  EXPECT_EQ(arena.memory_usage(), usage);

  arena.allocate(12345);
  usage += 12345 + ptr_char_size;
  EXPECT_EQ(arena.memory_usage(), usage);
}
