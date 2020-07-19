#include <chrono>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>

int main(int argc, char* argv[]) {
  std::queue<int> produced_nums;
  std::mutex mtx;
  std::condition_variable cv;
  bool notified = false;

  // 生产者
  auto producer = [&]() {
    for (int i = 0;; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(900));
      std::unique_lock<std::mutex> lock(mtx);
      std::cout << "producing " << i << std::endl;
      produced_nums.push(i);
      notified = true;
      cv.notify_all();
    }
  };

  // 消费者
  auto consumer = [&]() {
    while (true) {
      std::unique_lock<std::mutex> lock(mtx);
      while (!notified) {
        cv.wait(lock);
      }
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      lock.lock();
      while (!produced_nums.empty()) {
        std::cout << "consuming " << produced_nums.front() << std::endl;
        produced_nums.pop();
      }
      notified = false;
    }
  };

  // 分别在不同的线程中运行
  std::thread p(producer);
  std::thread cs[2];
  for (int i = 0; i < 2; i++) {
    cs[i] = std::thread(consumer);
  }
  p.join();
  for (int i = 0; i < 2; i++) {
    cs[i].join();
  }
  return 0;
}
