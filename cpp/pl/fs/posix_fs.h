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
#include <fcntl.h>
#include <unistd.h>

namespace pl {

// 64K
constexpr const size_t WRITE_BUFFER_SIZE = 65536;
constexpr const int OPEN_BASE_FLAGS = O_CLOEXEC;

Status posixError(const std::string& context, int err_number);

class PosixFsWriter final : public FsWriter {
public:
    PosixFsWriter(const PosixFsWriter&) = delete;
    PosixFsWriter(PosixFsWriter&&) = delete;
    PosixFsWriter& operator=(const PosixFsWriter&) = delete;
    PosixFsWriter& operator=(PosixFsWriter&&) = delete;

    PosixFsWriter(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {
        assert(fd >= 0);
    }

    ~PosixFsWriter() override {
        if (fd_ >= 0) {
            close();
        }
    }

    Status append(const Binary& data) override;
    Status close() override;
    Status flush() override;
    Status sync() override;

private:
    Status writeUnbuffered(const char* data, std::size_t size);
    Status flushBuffer();
    Status syncFd(int fd, const std::string& filename);

private:
    char buf_[WRITE_BUFFER_SIZE];
    std::size_t pos_{0};
    int fd_{-1};
    const std::string filename_;
};

class PosixFsReader final : public FsReader {
public:
    PosixFsReader(const PosixFsReader&) = default;
    PosixFsReader(PosixFsReader&&) = default;
    PosixFsReader& operator=(const PosixFsReader&) = delete;
    PosixFsReader& operator=(PosixFsReader&&) = delete;

    PosixFsReader(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {
        assert(fd >= 0);
    }

    ~PosixFsReader() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    };

    [[nodiscard]] std::size_t size() const override;

    Status read(uint64_t offset, std::size_t n, Binary* result, char* scratch) const override;

private:
    const std::string filename_;
    int fd_{-1};
};

class PosixFs : public Fs {
public:
    PosixFs() = default;
    FsWriterPtr newFsWriter(const std::string& filename, Status* status) override;
    FsReaderPtr newFsReader(const std::string& filename, Status* status) override;
};

} // namespace pl
