#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "bloom_filter.h"

std::string random_string(size_t length)
{
  auto randchar = []() -> char {
    const char charset[] = "0123456789"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                           "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  std::string str(length, 0);
  std::generate_n(str.begin(), length, randchar);
  return str;
}

int main(int argc, char* argv[])
{
  highkyck::bloom::BloomFilter filter(16 * 1024 * 1024 * 8);

  {
    const char* data = "liubang";
    uint64_t len = strlen(data);
    filter.insert(data, len);
    auto res = filter.contains(data, len);
    std::cout << res << '\n';
  }

  {
    const char* data = "other string";
    uint64_t len = strlen(data);
    auto res = filter.contains(data, len);
    std::cout << res << '\n';
  }

  {
    std::vector<std::string> strs;
    for (int i = 1; i < 100; ++i) {
      auto str = random_string(i);
      strs.push_back(str);
      filter.insert(str.c_str(), str.size());
    }

    for (auto& str : strs) {
      auto ret = filter.contains(str.data(), str.size());
      if (!ret) { std::cout << "[ERROR]: " << str << '\n'; }
    }
  }

  return 0;
}
