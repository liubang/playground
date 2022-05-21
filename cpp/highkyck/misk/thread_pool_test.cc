#include "includes/thread_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <utility>
#include <vector>

TEST(thread_pool, thread_pool)
{
  ThreadPool pool(1024);
  std::vector<std::future<std::pair<int, int>>> results;

  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < 1024; ++i) {
    results.emplace_back(pool.enqueue([i] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return std::make_pair(i, i);
    }));
  }

  for (auto &result : results) {
    std::pair<int, int> pair = result.get();
    EXPECT_EQ(pair.first, pair.second);
  }

  auto duration =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);

  EXPECT_LT(duration.count(), 100);
}
