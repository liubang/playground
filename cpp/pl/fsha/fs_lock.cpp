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

#include <string>

namespace pl {

bool FsLock::lock() {

}

bool FsLock::try_lock() {
    std::string indicator_path = lock_name_ + '/' + indicator_;
    if (fs_->utime(indicator_path, -1) && extend()) {
        return true;
    }

    uint64_t flag = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH;

    auto st = fs_->mkdir(lock_name_, flag);
    if (!st.IsOk()) {
        return false;
    }
    st = fs_->mkdir(indicator_path, flag);
    if (!st.IsOk()) {
        fs_->remove(lock_name_);
        return false;
    }
    return true;
}

bool FsLock::unlock() {
    if (!check_lock_holder()) {
        return false;
    }
    std::string indicator_path = lock_name_ + '/' + indicator_;
    auto st = fs_->remove(indicator_path);
    if (!st.IsOk()) {
        return false;
    }
    st = fs_->remove(lock_name_);
    return st.IsOk();
}

bool FsLock::extend() {
    if (check_lock_holder()) {
        auto st = fs_->utime(lock_name_, -1);
        if (!st.IsOk()) {
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
    return st.IsOk() && ok;
}

} // namespace pl
