#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

std::once_flag flag;
std::atomic<int64_t> counter = 0;

void foo() {
  std::call_once(flag, []() {
    counter++;
    std::cout << "test do once" << std::endl;
  });
}

TEST(misk, test_call_once) {
  std::vector<std::thread> threads;
  for (size_t i = 0; i < 10; ++i) {
    threads.emplace_back(foo);
  }
  for (size_t i = 0; i < 10; ++i) {
    threads[i].join();
  }

  EXPECT_EQ(counter, 1);
}
