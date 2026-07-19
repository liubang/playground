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
// Created: 2026/07/17

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minidfs/client/dfs_output_stream.h"
#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/sstv2/io/filesystem.h"

namespace pl::minidfs {
class DfsClient;
} // namespace pl::minidfs

namespace pl::sstv2::io {

class MiniDfsFileSystem final : public FileSystem {
public:
    explicit MiniDfsFileSystem(std::shared_ptr<minidfs::DfsClient> client)
        : client_(std::move(client)) {}

    [[nodiscard]] absl::StatusOr<FileHandle> create(std::string_view path,
                                                    const CreateOptions& options = {}) override;
    [[nodiscard]] absl::StatusOr<FileHandle> open(std::string_view path) override;
    [[nodiscard]] absl::StatusOr<FileHandle> open(std::string_view path,
                                                  const FileIdentity& expected) override;
    [[nodiscard]] absl::Status append(FileHandle handle, std::span<const std::byte> data) override;
    [[nodiscard]] absl::Status read_at(FileHandle handle,
                                       uint64_t offset,
                                       std::span<std::byte> destination) override;
    [[nodiscard]] absl::StatusOr<uint64_t> size(FileHandle handle) override;
    [[nodiscard]] absl::StatusOr<FileIdentity> close(FileHandle handle) override;
    [[nodiscard]] absl::Status remove(std::string_view path) override;
    [[nodiscard]] absl::Status remove(std::string_view path, const FileIdentity& expected) override;
    [[nodiscard]] absl::Status rename(std::string_view source,
                                      std::string_view destination) override;

private:
    struct ReadFile {
        std::string path;
        minidfs::FileIdentity identity;
    };
    using OpenFile = std::variant<minidfs::DfsOutputStream, ReadFile>;

    std::shared_ptr<minidfs::DfsClient> client_;
    std::mutex mutex_;
    std::unordered_map<FileHandle, std::unique_ptr<OpenFile>> open_files_;
};

} // namespace pl::sstv2::io
