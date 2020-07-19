#include <iostream>
#include <string>
#include <thread>

using namespace std;

int main(int argc, char *argv[]) {
  std::thread t1([] {
    for (int i = 0; i < 100; i++) {
      std::cout << "t1:" << i << std::endl;
    }
  });
  t1.join();
  return 0;
}
