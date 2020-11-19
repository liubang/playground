#include "thread-pool.h"
#include <memory>
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
  auto pool = std::make_shared<FixedThreadPool>(100);
  for (int i = 0; i < 10; ++i) {
    pool->execute([i]() {
      std::cout << i << " - hello world\n" << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    });
  }
  return 0;
}
