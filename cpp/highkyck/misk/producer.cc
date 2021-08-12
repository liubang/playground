#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>

extern ::std::condition_variable cond;
extern ::std::mutex mtx;
extern ::std::queue<::std::shared_ptr<int>> queue;

void produce(::std::shared_ptr<int>&& g)
{
  ::std::lock_guard<::std::mutex> lk(mtx);
  queue.push(::std::move(g));
  cond.notify_one();
}
