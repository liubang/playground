#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
  std::atomic<int64_t> flag = 0;
  std::thread t1([&]() {
    flag++;
    std::cout << flag << std::endl;
  });

  std::thread t2([&]() {
    flag++;
    std::cout << flag << std::endl;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  t1.join();
  t2.join();
  return 0;
}
