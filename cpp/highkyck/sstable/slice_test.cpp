#include "slice.h"

#include <gtest/gtest.h>

TEST(Slice, create_slice)
{
  // 1. empty slice
  highkyck::sstable::Slice s1;
  EXPECT_EQ(s1.to_string(), "");
  EXPECT_EQ(s1.size(), 0);

  // 2. create a slice that refers to d[0, n - 1]
  const char ch1[] = { "hello world" };
  highkyck::sstable::Slice s2(ch1, 11);
  EXPECT_EQ(s2.data(), ch1);
  EXPECT_EQ(s2.to_string(), "hello world");
  EXPECT_EQ(s2.size(), 11);

  // 3. create a slice that refers to string
  const std::string str1 = "hello world";
  highkyck::sstable::Slice s3(str1);
  EXPECT_EQ(s3.to_string(), "hello world");
  EXPECT_EQ(s3.size(), 11);

  // 4. create a slice that refers to s[0, strlen(s) - 1]
  const char *ch2 = "hello world";
  highkyck::sstable::Slice s4(ch2);
  EXPECT_EQ(s4.data(), ch2);
  EXPECT_EQ(s4.to_string(), "hello world");
  EXPECT_EQ(s4.size(), 11);
}

TEST(Slice, operator)
{
  const std::string str = "hello world";
  highkyck::sstable::Slice s(str);
  for (std::size_t i = 0; i < str.size(); ++i) { EXPECT_EQ(s[i], str.at(i)); }
}

TEST(Slice, compare)
{
  const std::string str1 = "hello world";
  highkyck::sstable::Slice s1(str1);
  const std::string str2 = "hello world";
  highkyck::sstable::Slice s2(str2);
  EXPECT_EQ(s1.compare(s2), 0);
  EXPECT_TRUE(s1 == s2);

  const std::string str3 = "hello world 123";
  highkyck::sstable::Slice s3(str3);
  EXPECT_TRUE(s1.compare(s3) < 0);
  EXPECT_TRUE(s1 < s3);

  const std::string str4 = "hello world 456";
  highkyck::sstable::Slice s4(str4);
  EXPECT_TRUE(s4.compare(s3) > 0);
  EXPECT_TRUE(s4 > s3);
}

TEST(Slice, remove_prefix)
{
  const std::string str = "hello world";
  highkyck::sstable::Slice s(str);
  s.remove_prefix(5);

  EXPECT_EQ(s.to_string(), " world");
  EXPECT_EQ(s.size(), 6);
}

TEST(Slice, start_with)
{
  const std::string str = "hello world";
  highkyck::sstable::Slice s(str);

  const std::string str2 = "hello";
  highkyck::sstable::Slice s2(str2);
  EXPECT_TRUE(s.start_with(s2));

  highkyck::sstable::Slice s3("helll");
  EXPECT_FALSE(s.start_with(s3));
}

TEST(Slice, to_string)
{
  highkyck::sstable::Slice s("hello world");
  EXPECT_EQ(s.to_string(), "hello world");
}
