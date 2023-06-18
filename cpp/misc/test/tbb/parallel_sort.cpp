//=====================================================================
//
// parallel_sort.cpp -
//
// Created by liubang on 2023/06/18 17:08
// Last Modified: 2023/06/18 17:08
//
//=====================================================================
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "cpp/misc/test/tbb/tools.h"

int main(int argc, char *argv[]) {
  size_t n = 1 << 24;
  pl::measure([n] {
    std::vector<int> arr(n);
    std::generate(arr.begin(), arr.end(), std::rand);
    tbb::parallel_sort(arr.begin(), arr.end(), std::less<int>{});
  });
  return 0;
}
