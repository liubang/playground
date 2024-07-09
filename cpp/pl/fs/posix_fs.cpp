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
#include <sys/stat.h>
#include <unistd.h>

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
    int ret = ::access(path.data(), F_OK);
    int errnum = errno;
    if (ret != 0) {
        if (errnum == ENOENT) {
            *result = false;
            return Status::NewOk();
        }
        return Status::NewIOError();
    }
    *result = true;
    return Status::NewOk();
}

Status PosixFileSystem::rename(std::string_view old_path, std::string_view new_path) {
    return Status::NewOk();
}

Status PosixFileSystem::mkdir(std::string_view path, uint64_t flags) { return Status::NewOk(); }

Status PosixFileSystem::remove(std::string_view path) {
    struct stat buffer;
    int ret = ::stat(path.data(), &buffer);
    if (ret != 0) {
        return Status::NewIOError();
    }
    if (S_ISDIR(buffer.st_mode)) {
        // TODO: remove directory
    } else {
        ret = std::remove(path.data());
        if (ret != 0) {
            return Status::NewIOError();
        }
    }
    return Status::NewOk();
}

Status posixError(const std::string& context, int err_number) {
    if (errno == ENOENT) {
        return Status::NewNotFound(context + std::strerror(err_number));
    }
    return Status::NewIOError(context + std::strerror(err_number));
}

Status PosixFsWriter::append(std::string_view data) {
    std::size_t write_size = data.size();
    const char* write_data = data.data();
    std::size_t copy_size = std::min(write_size, WRITE_BUFFER_SIZE - pos_);
    std::memcpy(buf_ + pos_, write_data, copy_size);
    write_data += copy_size;
    write_size -= copy_size;
    pos_ += copy_size;
    if (write_size == 0) {
        return Status::NewOk();
    }

    auto status = flushBuffer();
    if (!status.isOk()) {
        return status;
    }

    if (write_size < WRITE_BUFFER_SIZE) {
        std::memcpy(buf_, write_data, write_size);
        pos_ = write_size;
        return Status::NewOk();
    }

    return writeUnbuffered(write_data, write_size);
}

Status PosixFsWriter::flush() { return flushBuffer(); }

Status PosixFsWriter::close() {
    Status status = flushBuffer();
    int result = ::close(fd_);
    if (result < 0 && status.isOk()) {
        status = posixError(filename_, errno);
    }
    fd_ = -1;
    return status;
}

Status PosixFsWriter::sync() {
    Status status = flushBuffer();
    if (!status.isOk()) {
        return status;
    }
    return syncFd(fd_, filename_);
}

Status PosixFsWriter::writeUnbuffered(const char* data, std::size_t size) {
    while (size > 0) {
        ssize_t write_result = ::write(fd_, data, size);
        if (write_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posixError(filename_, errno);
        }
        data += write_result;
        size -= write_result;
    }
    return Status::NewOk();
}

Status PosixFsWriter::flushBuffer() {
    Status status = writeUnbuffered(buf_, pos_);
    pos_ = 0;
    return status;
}

Status PosixFsWriter::syncFd(int fd, const std::string& filename) {
    // 这里简单粗暴的区分Linux和macos
#if defined(__APPLE__) && defined(__MACH__)
    if (::fsync(fd) == 0) {
#else
    if (::fdatasync(fd) == 0) {
#endif
        return Status::NewOk();
    }
    return posixError(filename, errno);
}

std::size_t PosixFsReader::size() const {
    if (fd_ >= 0) {
        struct stat file_stat;
        if (::fstat(fd_, &file_stat) == -1) {
            return 0;
        }
        return static_cast<std::size_t>(file_stat.st_size);
    }
    return 0;
}

Status PosixFsReader::read(uint64_t offset,
                           std::size_t n,
                           std::string_view* result,
                           char* scratch) const {
    Status status;
    ssize_t read_size = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    *result = std::string_view(scratch, (read_size < 0) ? 0 : read_size);
    if (read_size < 0) {
        status = posixError(filename_, errno);
    }
    return status;
}

FsWriterPtr PosixFs::newFsWriter(const std::string& filename, Status* status) {
    int fd = ::open(filename.c_str(), O_TRUNC | O_WRONLY | O_CREAT | OPEN_BASE_FLAGS, 0644);
    if (fd < 0) {
        *status = posixError(filename, errno);
        return nullptr;
    }
    *status = Status::NewOk();
    return std::make_unique<PosixFsWriter>(filename, fd);
}

FsReaderPtr PosixFs::newFsReader(const std::string& filename, Status* status) {
    int fd = ::open(filename.c_str(), O_RDONLY | OPEN_BASE_FLAGS);
    if (fd < 0) {
        *status = posixError(filename, errno);
        return nullptr;
    }
    *status = Status::NewOk();
    return std::make_unique<PosixFsReader>(filename, fd);
}

} // namespace pl
