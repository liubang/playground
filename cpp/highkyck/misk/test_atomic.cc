#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char *argv[])
{
  std::atomic<int64_t> flag = 0;

  std::vector<std::thread> threads;

  for (int i = 0; i < 32; ++i) {
    threads.emplace_back([&flag]() { flag.fetch_add(1, std::memory_order_relaxed); });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  for (int i = 0; i < 32; ++i) { threads[i].join(); }

  std::cout << flag.load(std::memory_order_relaxed) << std::endl;
  return 0;
}
