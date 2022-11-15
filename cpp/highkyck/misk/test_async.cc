#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace highkyck {
void funcA() {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::cout << __FUNCTION__ << std::endl;
}

void funcB() {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::cout << __FUNCTION__ << std::endl;
}
}  // namespace highkyck

int main(int argc, char* argv[]) {
  std::future<void> f1 = std::async(highkyck::funcA);
  std::future<void> f2 = std::async(highkyck::funcB);

  auto start = std::chrono::steady_clock::now();

  f1.get();
  f2.get();

  std::cout << "std::async 耗时："
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count()
            << "ms" << std::endl;

  start = std::chrono::steady_clock::now();
  std::thread t1(highkyck::funcA);
  std::thread t2(highkyck::funcB);

  t1.join();
  t2.join();

  std::cout << "std::thread 耗时："
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count()
            << "ms" << std::endl;

  return 0;
}
