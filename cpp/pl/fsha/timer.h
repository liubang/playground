// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#pragma once

#include <sys/time.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace pl {

inline int64_t gettimeofday_us() {
    timeval now;
    ::gettimeofday(&now, nullptr);
    return now.tv_sec * 1000000L + now.tv_usec;
}

inline int64_t gettimeofday_ms() { return gettimeofday_us() / 1000L; }

inline int64_t gettimeofday_s() { return gettimeofday_us() / 1000000L; }

class Timer final {
public:
    Timer() = default;

    ~Timer() { stop(); }

    void start(std::function<void()> task, std::chrono::milliseconds interval) {
        running_ = true;
        worker_ = std::thread([this, task, interval]() {
            while (running_) {
                auto next_term = std::chrono::steady_clock::now() + interval;
                task();
                std::this_thread::sleep_until(next_term);
            }
        });
    }

    void stop() {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace pl
