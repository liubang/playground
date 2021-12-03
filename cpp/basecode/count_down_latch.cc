#include "count_down_latch.h"

#include <chrono>

namespace basecode {

CountDownLatch::CountDownLatch(uint32_t count)
    : count_(count)
{}

void CountDownLatch::await(uint64_t nanosecs)
{
    std::unique_lock<std::mutex> lk(mutex_);
    if (0 == count_) {
        return;
    }
    if (nanosecs > 0) {
        cv_.wait_for(lk, std::chrono::nanoseconds(nanosecs));
    } else {
        cv_.wait(lk);
    }
}

uint32_t CountDownLatch::get_count()
{
    std::unique_lock<std::mutex> lk(mutex_);
    return count_;
}

void CountDownLatch::count_down()
{
    std::unique_lock<std::mutex> lk(mutex_);
    if (0 == count_) {
        return;
    }
    --count_;
    if (0 == count_) {
        cv_.notify_all();
    }
}

}  // namespace basecode
