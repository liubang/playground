#include <iostream>
#include <thread>

int main(int argc, char* argv[]) {
  std::thread t1([]() { std::cout << "thread1" << std::endl; });
  std::thread t2([]() { std::cout << "thread2" << std::endl; });

  t1.join();
  t2.join();
  std::cout << "done" << std::endl;
  return 0;
}
