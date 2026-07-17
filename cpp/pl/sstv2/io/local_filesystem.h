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

#pragma once

#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "cpp/pl/sstv2/io/filesystem.h"

namespace pl::sstv2::io {

class LocalFileSystem final : public FileSystem {
public:
    ~LocalFileSystem() override;

    [[nodiscard]] absl::StatusOr<FileHandle> create(std::string_view path,
                                                    const CreateOptions& options = {}) override;
    [[nodiscard]] absl::StatusOr<FileHandle> open(std::string_view path) override;
    [[nodiscard]] absl::Status append(FileHandle handle, std::span<const std::byte> data) override;
    [[nodiscard]] absl::Status read_at(FileHandle handle,
                                       uint64_t offset,
                                       std::span<std::byte> destination) override;
    [[nodiscard]] absl::StatusOr<uint64_t> size(FileHandle handle) override;
    [[nodiscard]] absl::Status close(FileHandle handle) override;
    [[nodiscard]] absl::Status remove(std::string_view path) override;
    [[nodiscard]] absl::Status rename(std::string_view source,
                                      std::string_view destination) override;

private:
    struct OpenFile {
        int descriptor = -1;
        bool writable = false;
        std::mutex mutex;
    };

    [[nodiscard]] absl::StatusOr<std::shared_ptr<OpenFile>> get_open_file(FileHandle handle) const;

    mutable std::mutex mutex_;
    std::unordered_map<FileHandle, std::shared_ptr<OpenFile>> open_files_;
};

} // namespace pl::sstv2::io
