#include <atomic>
#include <iostream>
#include <thread>

std::atomic<int> count = {0};

int main(int argc, char* argv[]) {
  std::thread t1([]() { count.fetch_add(1); });

  std::thread t2([]() {
    count++; // 等价于 fetch_add
    count += 1; // 等价于 fetch_add
  });

  t1.join();
  t2.join();

  std::cout << count << std::endl;

  return 0;
}
