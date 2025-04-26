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

#include "cpp/pl/status/result.h"
#include "cpp/pl/utility/utility.h"

#include <cstdint>
#include <ctime>
#include <memory>

namespace pl {

class FileDescriptor : public DisableCopyAndMove {
public:
    FileDescriptor() = default;
    virtual ~FileDescriptor() = default;
};

using FileDescriptorRef = std::shared_ptr<FileDescriptor>;

class FileSystem : public DisableCopyAndMove {
public:
    FileSystem() = default;
    virtual ~FileSystem() = default;

    virtual Result<Void> open(std::string_view path, uint64_t flags, FileDescriptorRef* fd) = 0;

    virtual Result<Void> close(const FileDescriptorRef& fd) = 0;

    virtual Result<Void> pread(const FileDescriptorRef& fd,
                               uint64_t offset,
                               std::size_t n,
                               const char* buffer,
                               std::string_view* result) = 0;

    virtual Result<Void> append(const FileDescriptorRef& fd,
                                uint64_t flags,
                                std::string_view data) = 0;

    virtual Result<Void> fsync(const FileDescriptorRef& fd, uint64_t flags) = 0;

    virtual Result<Void> size(std::string_view path, uint64_t* result) = 0;

    virtual Result<Void> size(const FileDescriptorRef& fd, uint64_t* result) = 0;

    virtual Result<Void> mtime(std::string_view path, std::time_t* result) = 0;

    virtual Result<Void> mtime(const FileDescriptorRef& fd, std::time_t* result) = 0;

    virtual Result<Void> exist(std::string_view path, bool* result) = 0;

    virtual Result<Void> isdir(std::string_view path, bool* result) = 0;

    virtual Result<Void> rename(std::string_view old_path, std::string_view new_path) = 0;

    virtual Result<Void> mkdir(std::string_view path, uint64_t flags) = 0;

    virtual Result<Void> remove(std::string_view path) = 0;

    virtual Result<Void> utime(std::string_view path, time_t set_time) = 0;
};

using FileSystemPtr = std::unique_ptr<FileSystem>;
using FileSystemRef = std::shared_ptr<FileSystem>;

} // namespace pl
