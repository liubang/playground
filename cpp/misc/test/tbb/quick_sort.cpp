//=====================================================================
//
// quick_sort.cpp -
//
// Created by liubang on 2023/06/18 16:59
// Last Modified: 2023/06/18 16:59
//
//=====================================================================

#include "cpp/misc/test/tbb/quick_sort.h"

#include <algorithm>
#include <iostream>
#include <vector>

#include "cpp/misc/test/tbb/tools.h"

int main(int argc, char *argv[]) {
  size_t n = 1 << 24;

  pl::measure([n] {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    std::sort(arr.begin(), arr.end(), std::less<int>{});
  });

  pl::measure([n] {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort(arr.data(), arr.size());
  });

  pl::measure([n] {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort2(arr.data(), arr.size());
  });

  pl::measure([n] {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    pl::quick_sort3(arr.data(), arr.size());
  });

  return 0;
}
