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

#include "cpp/misc/fs/posix_fs.h"

#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <string>

namespace pl {

Status posixError(const std::string& context, int err_number) {
    if (errno == ENOENT) {
        return Status::NewNotFound(context + std::strerror(err_number));
    }
    return Status::NewIOError(context + std::strerror(err_number));
}

Status PosixFsWriter::append(const Binary& data) {
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

Status PosixFsReader::read(uint64_t offset, std::size_t n, Binary* result, char* scratch) const {
    Status status;
    ssize_t read_size = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    result->reset(scratch, (read_size < 0) ? 0 : read_size);
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
