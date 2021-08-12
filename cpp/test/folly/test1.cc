#include <folly/Conv.h>
#include <folly/Synchronized.h>
#include <iostream>
#include <string>
#include <unordered_map>

namespace test {
folly::Synchronized<std::unordered_map<std::string, std::string>> objs;
}

int main(int argc, char* argv[])
{
  test::objs.withWLock([](auto& mmp) {
    for (int32_t i = 0; i < 100; ++i) {
      std::string key = folly::to<std::string>("hello", i);
      mmp[key] = key;
    }
  });

  auto mmp = test::objs.withWLock([](auto& mmp) {
    auto mmmp = mmp;
    mmp.clear();
    return mmmp;
  });

  std::cout << "outter" << '\n';
  for (auto& m : mmp) { std::cout << m.first << '\n'; }

  test::objs.withRLock([](auto& mmp) {
    std::cout << "inner" << '\n';
    for (auto& m : mmp) { std::cout << m.first << '\n'; }
  });

  return 0;
}
