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

#include "cpp/pl/fsha/fs_adaptor.h"

namespace pl {

class LocalFile : public FileAdaptor {
public:
    LocalFile(int fd, const std::string& name, ssize_t size)
        : FileAdaptor(name), fd_(fd), size_(size) {}
    virtual ~LocalFile() = default;

    ssize_t size() const override;
    ssize_t append(const void* buf, const size_t count) override;
    ssize_t read(void* buf, const size_t count, int64_t offset, bool* is_eof) override;
    int clear() override;
    int close() override;
    int sync() override;

private:
private:
    int fd_;
    ssize_t size_;
    bool direct_io_{false};
};

class LocalDir : public DirAdaptor {};

class LocalFs : public FsAdaptor {};

} // namespace pl
