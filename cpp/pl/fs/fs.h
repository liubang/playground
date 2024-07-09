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

#include "cpp/pl/status/status.h"

#include <cstdint>
#include <ctime>
#include <memory>

namespace pl {

class FileDescriptor {
public:
    FileDescriptor() = default;
    virtual ~FileDescriptor() = default;

    // disable copy and move
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;
};

using FileDescriptorRef = std::shared_ptr<FileDescriptor>;

class FileSystem {
public:
    FileSystem() = default;
    virtual ~FileSystem() = default;

    // disable copy and move
    FileSystem(const FileSystem&) = delete;
    FileSystem(FileSystem&&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;
    FileSystem& operator=(FileSystem&&) = delete;

    virtual Status open(std::string_view path, uint64_t flags, FileDescriptorRef* fd) = 0;

    virtual Status close(const FileDescriptorRef& fd) = 0;

    virtual Status pread(const FileDescriptorRef& fd,
                         uint64_t offset,
                         std::size_t n,
                         const char* buffer,
                         std::string_view* result) = 0;

    virtual Status append(const FileDescriptorRef& fd, uint64_t flags, std::string_view data) = 0;

    virtual Status fsync(const FileDescriptorRef& fd, uint64_t flags) = 0;

    virtual Status size(std::string_view path, uint64_t* result) = 0;

    virtual Status size(const FileDescriptorRef& fd, uint64_t* result) = 0;

    virtual Status mtime(std::string_view path, std::time_t* result) = 0;

    virtual Status mtime(const FileDescriptorRef& fd, std::time_t* result) = 0;

    virtual Status exist(std::string_view path, bool* result) = 0;

    virtual Status rename(std::string_view old_path, std::string_view new_path) = 0;

    virtual Status mkdir(std::string_view path, uint64_t flags) = 0;

    virtual Status remove(std::string_view path) = 0;
};

using FileSystemPtr = std::unique_ptr<FileSystem>;
using FileSystemRef = std::shared_ptr<FileSystem>;

class FsReader {
public:
    virtual ~FsReader() = default;

    virtual Status read(uint64_t offset,
                        size_t n,
                        std::string_view* result,
                        char* scratch) const = 0;

    [[nodiscard]] virtual std::size_t size() const = 0;
};

using FsReaderPtr = std::unique_ptr<FsReader>;
using FsReaderRef = std::shared_ptr<FsReader>;

class FsWriter {
public:
    FsWriter() = default;
    FsWriter(const FsWriter&) = delete;
    FsWriter& operator=(const FsWriter&) = delete;
    virtual ~FsWriter() = default;
    virtual Status append(std::string_view data) = 0;
    virtual Status close() = 0;
    virtual Status flush() = 0;
    virtual Status sync() = 0;
};

using FsWriterPtr = std::unique_ptr<FsWriter>;
using FsWriterRef = std::shared_ptr<FsWriter>;

class Fs {
public:
    Fs() = default;
    Fs(const Fs&) = delete;
    Fs& operator=(const Fs&) = delete;
    virtual ~Fs() = default;
    virtual FsReaderPtr newFsReader(const std::string& filename, Status* status) = 0;
    virtual FsWriterPtr newFsWriter(const std::string& filename, Status* status) = 0;
};

using FsPtr = std::unique_ptr<Fs>;
using FsRef = std::shared_ptr<Fs>;

} // namespace pl
