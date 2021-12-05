#pragma once

#include <atomic>
#include <mutex>

namespace highkyck {
namespace misk {
template<class T>
class Singleton
{
public:
  T* getInstance()
  {
    if (ins_ == nullptr) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (ins_ == nullptr) { ins_ = new T; }
    }
    return ins_;
  }

private:
  std::atomic<T*> ins_{nullptr};
  std::mutex mutex_;
};
}  // namespace misk
}  // namespace highkyck
