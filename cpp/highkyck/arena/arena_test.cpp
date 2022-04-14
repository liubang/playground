#include "arena.h"

#include <gtest/gtest.h>

TEST(arena, allocate)
{
  constexpr std::size_t ptr_char_size = sizeof(char *);

  highkyck::Arena arena;
  arena.allocate(1024);
  int usage = 4096 + ptr_char_size;
  EXPECT_EQ(arena.memory_usage(), usage);

  arena.allocate(2048);
  EXPECT_EQ(arena.memory_usage(), usage);

  arena.allocate(2048);
  usage += 2048 + ptr_char_size;
  EXPECT_EQ(arena.memory_usage(), usage);
}
