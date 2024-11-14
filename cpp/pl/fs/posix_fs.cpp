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

#include "cpp/pl/fs/posix_fs.h"
#include "cpp/pl/log/logger.h"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cassert>
#include <string>

namespace pl {

class PosixFileDescriptor final : public FileDescriptor {
public:
    PosixFileDescriptor(int fd, std::string_view file_path) : fd_(fd), file_path_(file_path) {}

    ~PosixFileDescriptor() override {
        if (fd_ != -1) {
            int ret = ::fsync(fd_);
            if (ret != 0) {
                LOG(WARN) << "fsync failed. fd: " << fd_ << ", file: " << file_path_
                          << ", errno: " << errno << ", message: " << std::strerror(errno);
            }

            ret = ::close(fd_);
            if (ret != 0) {
                LOG(WARN) << "close failed. fd: " << fd_ << ", file: " << file_path_
                          << ", errno: " << errno << ", message: " << std::strerror(errno);
            }
        }
        fd_ = -1;
    }

private:
    int fd_{-1};
    std::string file_path_;

    friend class PosixFileSystem;
};

Status PosixFileSystem::open(std::string_view path, uint64_t flags, FileDescriptorRef* fd) {
    // TODO use custom flags
    // open for write
    int ret = 0;
    if ((flags & O_WRONLY) == O_WRONLY) {
        ret = ::open(path.data(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    } else {
        ret = ::open(path.data(), flags);
    }
    if (ret == -1) {
        LOG(WARN) << "open failed. path: " << path << ", errno: " << errno
                  << ", message: " << std::strerror(errno);
        return Status::NewIOError();
    }
    auto posix_fd = std::make_shared<PosixFileDescriptor>(ret, path);
    *fd = std::move(posix_fd);
    return Status::NewOk();
}

Status PosixFileSystem::close(const FileDescriptorRef& fd) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    if (posix_fd->fd_ != -1) {
        int ret = ::fsync(posix_fd->fd_);
        if (ret != 0) {
            LOG(WARN) << "fsync failed. fd: " << posix_fd->fd_ << ", file: " << posix_fd->file_path_
                      << ", errno: " << errno << ", message: " << std::strerror(errno);
            return Status::NewIOError();
        }

        ret = ::close(posix_fd->fd_);
        if (ret != 0) {
            LOG(WARN) << "close failed. fd: " << posix_fd->fd_ << ", file: " << posix_fd->file_path_
                      << ", errno: " << errno << ", message: " << std::strerror(errno);
            return Status::NewIOError();
        }
    }
    return Status::NewOk();
}

Status PosixFileSystem::pread(const FileDescriptorRef& fd,
                              uint64_t offset,
                              std::size_t n,
                              const char* buffer,
                              std::string_view* result) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    uint64_t read_count = 0;
    while (read_count < n) {
        ssize_t len = ::pread(posix_fd->fd_, (void*)(buffer + read_count), n - read_count,
                              offset + read_count);
        if (len == -1) {
            // TODO:
            LOG(WARN) << "failed pread, fd: " << posix_fd->fd_ << ", file: " << posix_fd->file_path_
                      << ", errno: " << errno << ", message: " << std::strerror(errno);
            return Status::NewIOError();
        }
        read_count += len;
        if (len == 0) {
            break;
        }
    }
    *result = std::string_view(buffer, read_count);
    return Status::NewOk();
}

Status PosixFileSystem::append(const FileDescriptorRef& fd, uint64_t flags, std::string_view data) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    std::size_t write_size = data.size();
    const char* write_data = data.data();
    uint64_t writed_size = 0;
    while (writed_size < write_size) {
        ssize_t len = ::write(posix_fd->fd_, write_data + writed_size, write_size - writed_size);
        if (len == -1) {
            // TODO: linux system errno
            return Status::NewIOError();
        }
        writed_size += len;
    }
    return Status::NewOk();
}

Status PosixFileSystem::fsync(const FileDescriptorRef& fd, uint64_t flags) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    int ret = ::fsync(posix_fd->fd_);
    if (ret != 0) {
        // TODO: linux system errno
        return Status::NewIOError();
    }
    return Status::NewOk();
}

Status PosixFileSystem::size(std::string_view path, uint64_t* result) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        // TODO: linux system errno
        return Status::NewIOError();
    }
    *result = static_cast<uint64_t>(buffer.st_size);
    return Status::NewOk();
}

Status PosixFileSystem::size(const FileDescriptorRef& fd, uint64_t* result) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    struct stat buffer;
    int ret = ::fstat(posix_fd->fd_, &buffer);
    if (ret != 0) {
        // TODO: linux system errno
        return Status::NewIOError();
    }
    *result = static_cast<uint64_t>(buffer.st_size);
    return Status::NewOk();
}

Status PosixFileSystem::mtime(std::string_view path, std::time_t* result) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        // TODO: linux system error
        return Status::NewIOError();
    }
    *result = buffer.st_mtime;
    return Status::NewOk();
}

Status PosixFileSystem::mtime(const FileDescriptorRef& fd, std::time_t* result) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    struct stat buffer;
    int ret = ::fstat(posix_fd->fd_, &buffer);
    if (ret != 0) {
        // TODO: linux system error
        return Status::NewIOError();
    }
    *result = buffer.st_mtime;
    return Status::NewOk();
}

Status PosixFileSystem::exist(std::string_view path, bool* result) {
    *result = std::filesystem::exists(path);
    return Status::NewOk();
}

Status PosixFileSystem::isdir(std::string_view path, bool* result) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        *result = false;
        return Status::NewIOError();
    }
    *result = S_ISDIR(statbuf.st_mode);
    return Status::NewOk();
}

Status PosixFileSystem::rename(std::string_view old_path, std::string_view new_path) {
    std::filesystem::rename(old_path, new_path);
    return Status::NewOk();
}

Status PosixFileSystem::mkdir(std::string_view path, uint64_t flags) {
    if (::mkdir(path.data(), flags) != 0) {
        return Status::NewIOError();
    }
    return Status::NewOk();
}

Status PosixFileSystem::remove(std::string_view path) {
    std::error_code err;
    bool ret = std::filesystem::remove_all(path, err);
    if (!ret) {
        return Status::NewIOError();
    }
    return Status::NewOk();
}

Status PosixFileSystem::utime(std::string_view path, time_t set_time) {
    struct utimbuf utimeb;
    struct utimbuf* utimeb_p = nullptr;
    if (set_time != -1) {
        utimeb.actime = time(nullptr);
        utimeb.modtime = set_time;
        utimeb_p = &utimeb;
    }
    if (::utime(path.data(), utimeb_p) != 0) {
        return Status::NewIOError();
    }
    return Status::NewOk();
}

} // namespace pl
