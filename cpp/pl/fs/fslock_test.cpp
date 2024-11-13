#include "fslock.h"

#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>

namespace {
int64_t rand_int(int64_t min, int64_t max) {
    std::srand(std::time(nullptr));
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> distribution(min, max);
    return distribution(generator);
}
} // namespace

TEST(fslock, test) {
    std::vector<std::thread> threads;
    int sum = 0;
    int concurrent = rand_int(3, 33);
    for (int i = 0; i < concurrent; ++i) {
        threads.emplace_back([i, &sum]() {
            pl::FsLock lock("./test.lock");
            if (!lock.lock()) {
                ::printf("[%d] failed to get lock\n", i);
                EXPECT_TRUE(false);
            } else {
                ::printf("[%d] success to get lock\n", i);
            }

            ::printf("[%d] do something long time...\n", i);
            std::this_thread::sleep_for(std::chrono::milliseconds(rand_int(10, 1000)));
            ::printf("[%d] finished do something long time...\n", i);
            sum++;

            if (!lock.unlock()) {
                ::printf("[%d] failed to release lock\n", i);
                EXPECT_TRUE(false);
            } else {
                ::printf("[%d] success to release lock\n", i);
            }
        });
    }

    for (int i = 0; i < concurrent; ++i) {
        threads[i].join();
    }

    EXPECT_EQ(concurrent, sum);
}
