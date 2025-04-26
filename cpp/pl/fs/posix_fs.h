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

#include <cassert>
#include <fcntl.h>
#include <unistd.h>

namespace pl {

class PosixFileSystem final : public FileSystem {
public:
    ~PosixFileSystem() override = default;

    Result<Void> open(std::string_view path, uint64_t flags, FileDescriptorRef* fd) override;

    Result<Void> close(const FileDescriptorRef& fd) override;

    Result<Void> pread(const FileDescriptorRef& fd,
                       uint64_t offset,
                       std::size_t n,
                       const char* buffer,
                       std::string_view* result) override;

    Result<Void> append(const FileDescriptorRef& fd,
                        uint64_t flags,
                        std::string_view data) override;

    Result<Void> fsync(const FileDescriptorRef& fd, uint64_t flags) override;

    Result<Void> size(std::string_view path, uint64_t* result) override;

    Result<Void> size(const FileDescriptorRef& fd, uint64_t* result) override;

    Result<Void> mtime(std::string_view path, std::time_t* result) override;

    Result<Void> mtime(const FileDescriptorRef& fd, std::time_t* result) override;

    Result<Void> exist(std::string_view path, bool* result) override;

    Result<Void> isdir(std::string_view path, bool* result) override;

    Result<Void> rename(std::string_view old_path, std::string_view new_path) override;

    Result<Void> mkdir(std::string_view path, uint64_t flags) override;

    Result<Void> remove(std::string_view path) override;

    Result<Void> utime(std::string_view path, time_t set_time) override;
};

} // namespace pl
