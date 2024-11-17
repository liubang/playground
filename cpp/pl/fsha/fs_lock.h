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

#include "cpp/pl/fs/fs.h"
#include "cpp/pl/fsha/timer.h"

#include <sys/stat.h>

#include <string>
#include <string_view>
#include <thread>

namespace pl {

class FsLock final {
    constexpr static std::chrono::milliseconds LOCK_EXTEND_INTERVAL =
        std::chrono::milliseconds(5000);

public:
    explicit FsLock(std::unique_ptr<FileSystem> fs,
                    std::string_view lock_name,
                    std::string_view indicator)
        : fs_(std::move(fs)), lock_name_(lock_name), indicator_(indicator) {
        timer_ = std::make_unique<Timer>();
    }

    ~FsLock() {};

    bool lock();

    bool try_lock();

    bool unlock();

private:
    bool extend();

    bool check_lock_holder();

    bool extend_lock();

    bool get_current_lock_holder(std::string* lock_holder);

private:
    std::string lock_name_;
    std::string indicator_;
    std::unique_ptr<FileSystem> fs_;
    std::unique_ptr<Timer> timer_;
};

} // namespace pl
