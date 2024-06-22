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
#include <memory>

namespace pl {

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
