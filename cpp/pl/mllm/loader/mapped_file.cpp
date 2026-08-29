// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/08/29 22:15

#include "cpp/pl/mllm/loader/mapped_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace pl::mllm {

Result<MappedFile> MappedFile::Open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::Error(ErrorCode::kNotFound, "open failed: " + path);
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return Status::Error(ErrorCode::kInvalidFormat, "fstat failed or empty file: " + path);
    }

    void* addr = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { // NOLINT(performance-no-int-to-ptr)
        ::close(fd);
        return Status::Error(ErrorCode::kInternal, "mmap failed: " + path);
    }
    ::madvise(addr, static_cast<size_t>(st.st_size), MADV_WILLNEED);

    MappedFile file;
    file.fd_ = fd;
    file.data_ = static_cast<const uint8_t*>(addr);
    file.size_ = static_cast<size_t>(st.st_size);
    file.path_ = path;
    return file;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = std::exchange(other.fd_, -1);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        path_ = std::move(other.path_);
    }
    return *this;
}

void MappedFile::Close() noexcept {
    if (data_ != nullptr) {
        ::munmap(const_cast<uint8_t*>(data_), size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

} // namespace pl::mllm
