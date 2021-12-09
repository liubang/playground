#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

std::once_flag flag;

void foo() {
  std::call_once(flag, []() { std::cout << "test do once" << std::endl; });
}

int main(int argc, char* argv[]) {
  std::vector<std::thread> threads;
  for (size_t i = 0; i < 10; ++i) {
    threads.emplace_back(foo);
  }
  for (size_t i = 0; i < 10; ++i) {
    threads[i].join();
  }

  std::cout << "done." << std::endl;

  return 0;
}
