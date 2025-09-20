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

#include "cpp/pl/status/result.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

namespace pl {

class PosixFileDescriptor final : public FileDescriptor {
public:
    PosixFileDescriptor(int fd, std::string_view file_path) : fd_(fd), file_path_(file_path) {}

    ~PosixFileDescriptor() override {
        if (fd_ != -1) {
            int ret = ::fsync(fd_);
            if (ret != 0) {
                XLOGF(WARN, "fsync failed, fd: {}, file: {}, errno: {}, message: {}", fd_,
                      file_path_, errno, std::strerror(errno));
            }

            ret = ::close(fd_);
            if (ret != 0) {
                XLOGF(WARN, "close failed, fd: {}, file: {}, errno: {}, message: {}", fd_,
                      file_path_, errno, std::strerror(errno));
            }
        }
        fd_ = -1;
    }

private:
    int fd_{-1};
    std::string file_path_;

    friend class PosixFileSystem;
};

Result<Void> PosixFileSystem::open(std::string_view path, uint64_t flags, FileDescriptorRef* fd) {
    // TODO use custom flags
    // open for write
    int ret = 0;
    if ((flags & O_WRONLY) == O_WRONLY) {
        ret = ::open(path.data(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    } else {
        ret = ::open(path.data(), flags);
    }
    if (ret == -1) {
        XLOGF(WARN, "open failed, path: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    auto posix_fd = std::make_shared<PosixFileDescriptor>(ret, path);
    *fd = std::move(posix_fd);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::close(const FileDescriptorRef& fd) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    if (posix_fd->fd_ != -1) {
        int ret = ::fsync(posix_fd->fd_);
        if (ret != 0) {
            XLOGF(WARN, "fsync failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
                  posix_fd->file_path_, errno, std::strerror(errno));
            return makeError(StatusCode::kIOError);
        }

        ret = ::close(posix_fd->fd_);
        if (ret != 0) {
            XLOGF(WARN, "close failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
                  posix_fd->file_path_, errno, std::strerror(errno));
            return makeError(StatusCode::kIOError);
        }
    }
    RETURN_VOID;
}

Result<Void> PosixFileSystem::pread(const FileDescriptorRef& fd,
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
            XLOGF(WARN, "pread failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
                  posix_fd->file_path_, errno, std::strerror(errno));
            return makeError(StatusCode::kIOError);
        }
        read_count += len;
        if (len == 0) {
            break;
        }
    }
    *result = std::string_view(buffer, read_count);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::append(const FileDescriptorRef& fd,
                                     uint64_t flags,
                                     std::string_view data) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    std::size_t write_size = data.size();
    const char* write_data = data.data();
    uint64_t writed_size = 0;
    while (writed_size < write_size) {
        ssize_t len = ::write(posix_fd->fd_, write_data + writed_size, write_size - writed_size);
        if (len == -1) {
            XLOGF(WARN, "append failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
                  posix_fd->file_path_, errno, std::strerror(errno));
            return makeError(StatusCode::kIOError);
        }
        writed_size += len;
    }
    RETURN_VOID;
}

Result<Void> PosixFileSystem::fsync(const FileDescriptorRef& fd, uint64_t flags) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    int ret = ::fsync(posix_fd->fd_);
    if (ret != 0) {
        XLOGF(WARN, "fsync failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
              posix_fd->file_path_, errno, std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    RETURN_VOID;
}

Result<Void> PosixFileSystem::size(std::string_view path, uint64_t* result) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        XLOGF(WARN, "stat failed,  file: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    *result = static_cast<uint64_t>(buffer.st_size);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::size(const FileDescriptorRef& fd, uint64_t* result) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    struct stat buffer;
    int ret = ::fstat(posix_fd->fd_, &buffer);
    if (ret != 0) {
        XLOGF(WARN, "fstat failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
              posix_fd->file_path_, errno, std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    *result = static_cast<uint64_t>(buffer.st_size);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::mtime(std::string_view path, std::time_t* result) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        XLOGF(WARN, "stat failed, file: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    *result = buffer.st_mtime;
    RETURN_VOID;
}

Result<Void> PosixFileSystem::mtime(const FileDescriptorRef& fd, std::time_t* result) {
    auto* posix_fd = static_cast<PosixFileDescriptor*>(fd.get());
    struct stat buffer;
    int ret = ::fstat(posix_fd->fd_, &buffer);
    if (ret != 0) {
        XLOGF(WARN, "fstat failed, fd: {}, file: {}, errno: {}, message: {}", posix_fd->fd_,
              posix_fd->file_path_, errno, std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    *result = buffer.st_mtime;
    RETURN_VOID;
}

Result<Void> PosixFileSystem::exist(std::string_view path, bool* result) {
    *result = std::filesystem::exists(path);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::isdir(std::string_view path, bool* result) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        *result = false;
        XLOGF(WARN, "stat failed, file: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    *result = S_ISDIR(buffer.st_mode);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::rename(std::string_view old_path, std::string_view new_path) {
    std::filesystem::rename(old_path, new_path);
    RETURN_VOID;
}

Result<Void> PosixFileSystem::mkdir(std::string_view path, uint64_t flags) {
    if (::mkdir(path.data(), flags) != 0) {
        XLOGF(WARN, "mkdir failed, file: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    RETURN_VOID;
}

Result<Void> PosixFileSystem::remove(std::string_view path) {
    std::error_code err;
    bool ret = std::filesystem::remove_all(path, err) != 0u;
    if (!ret) {
        XLOGF(WARN, "remove failed, file: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }
    RETURN_VOID;
}

Result<Void> PosixFileSystem::utime(std::string_view path, time_t set_time) {
    struct utimbuf utimeb;
    struct utimbuf* utimeb_p = nullptr;
    if (set_time != -1) {
        utimeb.actime = time(nullptr);
        utimeb.modtime = set_time;
        utimeb_p = &utimeb;
    }
    if (::utime(path.data(), utimeb_p) != 0) {
        XLOGF(WARN, "utime failed, file: {}, errno: {}, message: {}", path, errno,
              std::strerror(errno));
        return makeError(StatusCode::kIOError);
    }

    RETURN_VOID;
}

} // namespace pl
