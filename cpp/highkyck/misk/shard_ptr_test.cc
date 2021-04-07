#include <gtest/gtest.h>
#include <string>
#include <iostream>

#include "shard_ptr.h"

class Person {
 public:
  Person(const std::string& name, int age) : name_(name), age_(age) {}
  ~Person() = default;

  const std::string& getName() const {
    return name_;
  }

  int getAge() const {
    return age_;
  }

 private:
  std::string name_;
  int age_;
};

TEST(shard_ptr, shard_ptr) {
  shard_ptr<Person> p(new Person("test", 20));

  EXPECT_EQ("test", p->getName());
  EXPECT_EQ(20, p->getAge());
  EXPECT_EQ(1, p.getCount());
  auto p1 = p;

  EXPECT_EQ("test", p1->getName());
  EXPECT_EQ(20, p1->getAge());
  EXPECT_EQ(2, p.getCount());
  EXPECT_EQ(2, p1.getCount());
}
