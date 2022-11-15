#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

extern ::std::condition_variable cond;
extern ::std::mutex mtx;
extern ::std::queue<::std::shared_ptr<int>> queue;

::std::shared_ptr<int> consume() {
  ::std::unique_lock<::std::mutex> lk(mtx);
  while (queue.empty())
    cond.wait(lk);
  auto g = queue.front();
  queue.pop();
  return g;
}
