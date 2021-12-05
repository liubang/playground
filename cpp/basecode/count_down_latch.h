#pragma once

#include <condition_variable>
#include <mutex>

namespace basecode
{
class CountDownLatch
{
public:
  CountDownLatch(uint32_t count);

  void await(uint64_t nanosecs = 0);

  void count_down();

  uint32_t get_count();

private:
  CountDownLatch() = delete;

  CountDownLatch(const CountDownLatch& other) = delete;

  CountDownLatch& operator=(const CountDownLatch& other) = delete;

private:
  uint32_t count_;
  std::mutex mutex_;
  std::condition_variable cv_;
};
}  // namespace basecode
