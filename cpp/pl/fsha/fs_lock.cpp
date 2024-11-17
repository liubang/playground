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

#include "fs_lock.h"

namespace pl {

bool FsLock::lock() { return true; }

bool FsLock::try_lock() {
    std::string indicator_path = lock_name_ + '/' + indicator_;
    Status st = fs_->utime(indicator_path, -1);
    if (st.isOk() && extend()) {
        return true;
    }

    uint64_t flag = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH;

    st = fs_->mkdir(lock_name_, flag);
    if (!st.isOk()) {
        bool ok;
        st = fs_->isdir(lock_name_, &ok);
        if (!st.isOk() || !ok) {
            return false;
        }
        uint64_t modify_time = 0;
        st = fs_->mtime(lock_name_, &modify_time);
        if (!st.isOk() ||
            (((gettimeofday_s() - modify_time) * 1000) < (LOCK_EXTEND_INTERVAL.count() * 5))) {
            return false;
        }

        // get current lock holder
    }

    st = fs_->mkdir(indicator_path, flag);
    if (!st.isOk()) {
        fs_->remove(lock_name_);
        return false;
    }

    timer_->start(
        [this]() {
            extend();
        },
        LOCK_EXTEND_INTERVAL);

    return true;
}

bool FsLock::unlock() {
    timer_->stop();
    if (!check_lock_holder()) {
        return false;
    }
    std::string indicator_path = lock_name_ + '/' + indicator_;
    auto st = fs_->remove(indicator_path);
    if (!st.isOk()) {
        return false;
    }
    st = fs_->remove(lock_name_);
    return st.isOk();
}

bool FsLock::extend() {
    if (check_lock_holder()) {
        auto st = fs_->utime(lock_name_, -1);
        if (!st.isOk()) {
            return false;
        }
        return check_lock_holder();
    }
    return false;
}

bool FsLock::check_lock_holder() {
    std::string indicator_path = lock_name_ + '/' + indicator_;
    bool ok;
    auto st = fs_->isdir(indicator_path, &ok);
    return st.isOk() && ok;
}

} // namespace pl
