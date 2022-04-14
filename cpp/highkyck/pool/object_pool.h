#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace highkyck::pool {
class noncopyable
{
public:
  noncopyable(const noncopyable &) = delete;
  void operator=(const noncopyable &) = delete;

protected:
  noncopyable() = default;
  virtual ~noncopyable() = default;
};

template<class Obj, size_t N> class object_pool final : public noncopyable
{
public:
  using DeleterType = std::function<void(Obj *)>;

  static object_pool &get_instance()
  {
    static object_pool<Obj, N> pool;
    return pool;
  }

  std::unique_ptr<Obj, DeleterType> acquire()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (empty()) { return nullptr; }
    std::unique_ptr<Obj, DeleterType> ptr(
      pool_.front().release(), [this](Obj *o) { pool_.push_back(std::unique_ptr<Obj>(o)); });
    pool_.pop_front();
    return ptr;
  }

  bool empty() const { return pool_.empty(); }

  size_t size() const { return pool_.size(); }

private:
  object_pool()
  {
    static_assert(N < 500, "init size is out of range");
    for (size_t i = 0; i < N; ++i) { pool_.emplace_back(new Obj()); }
  }

private:
  std::deque<std::unique_ptr<Obj>> pool_;
  std::mutex mtx_;
};
}// namespace highkyck::pool
