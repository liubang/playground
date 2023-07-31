//=====================================================================
//
// quick_sort.cpp -
//
// Created by liubang on 2023/06/18 16:59
// Last Modified: 2023/06/18 16:59
//
//=====================================================================
#include <nanobench.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "cpp/misc/test/tbb/quick_sort.h"

constexpr size_t n = 1 << 10;

static void test1() {
  std::vector<int> arr(n);
  std::generate(arr.begin(), arr.end(), std::rand);
  std::sort(arr.begin(), arr.end(), std::less<int>{});
}

static void test2() {
  std::vector<int> arr(n);
  std::generate(arr.begin(), arr.end(), std::rand);
  pl::quick_sort(arr.data(), arr.size());
}

static void test3() {
  std::vector<int> arr(n);
  std::generate(arr.begin(), arr.end(), std::rand);
  pl::quick_sort2(arr.data(), arr.size());
}

static void test4() {
  std::vector<int> arr(n);
  std::generate(arr.begin(), arr.end(), std::rand);
  pl::quick_sort3(arr.data(), arr.size());
}

int main(int argc, char *argv[]) {
  ankerl::nanobench::Bench().run("test1", [&] { test1(); });
  ankerl::nanobench::Bench().run("test2", [&] { test2(); });
  ankerl::nanobench::Bench().run("test3", [&] { test3(); });
  ankerl::nanobench::Bench().run("test4", [&] { test4(); });
  return 0;
}
