#include <iostream>

#include "object_pool.h"

namespace {
struct TestObj
{
  std::string name;
  int32_t age;
};
};// namespace

int main(int argc, char *argv[])
{
  auto &pool = highkyck::pool::object_pool<TestObj, 10>::get_instance();

  {
    auto obj = pool.acquire();
    std::cout << pool.size() << '\n';
  }

  std::cout << pool.size() << '\n';

  return 0;
}
