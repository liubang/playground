#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
  std::vector<std::thread> ts;
  for (int i = 0; i < 10; ++i) {
    std::thread t([&] {
      for (int j = 0; j < 100; j++) {
        ::printf("[thread %d] - num: %d\n", i, j);
      }
    });
    ts.emplace_back(std::move(t));
  }
  for (auto& tt : ts) {
    tt.join();
  }
  return 0;
}
