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

#include "cpp/pl/sstv2/io/minidfs_filesystem.h"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "absl/strings/str_cat.h"
#include "cpp/pl/minidfs/client/dfs_client.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/status/status_code.h"

namespace pl::sstv2::io {
namespace {

absl::Status to_absl_status(const Status& status, std::string_view operation) {
    using minidfs::ErrorCode;
    const auto code = status.code();
    const std::string message = status.message().empty()
                                    ? std::string(operation)
                                    : absl::StrCat(operation, ": ", status.message());
    if (code == StatusCode::kInvalidArg ||
        code == static_cast<status_code_t>(ErrorCode::kInvalidArgument) ||
        code == static_cast<status_code_t>(ErrorCode::kInvalidPath) ||
        code == static_cast<status_code_t>(ErrorCode::kPathTooLong)) {
        return absl::InvalidArgumentError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kNotFound) ||
        code == static_cast<status_code_t>(ErrorCode::kFileNotFound) ||
        code == static_cast<status_code_t>(ErrorCode::kBlockNotFound)) {
        return absl::NotFoundError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kPermissionDenied)) {
        return absl::PermissionDeniedError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kAlreadyExists)) {
        return absl::AlreadyExistsError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kRPCTimeout)) {
        return absl::DeadlineExceededError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kRPCError) ||
        code == static_cast<status_code_t>(ErrorCode::kRPCConnectFailed) ||
        code == static_cast<status_code_t>(ErrorCode::kNoAvailableDataNode)) {
        return absl::UnavailableError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kFileUnderConstruction) ||
        code == static_cast<status_code_t>(ErrorCode::kLeaseExpired) ||
        code == static_cast<status_code_t>(ErrorCode::kLeaseConflict) ||
        code == static_cast<status_code_t>(ErrorCode::kLeaseNotFound)) {
        return absl::FailedPreconditionError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kChecksumMismatch) ||
        code == static_cast<status_code_t>(ErrorCode::kBlockCorrupt)) {
        return absl::DataLossError(message);
    }
    if (code == static_cast<status_code_t>(ErrorCode::kDiskFull)) {
        return absl::ResourceExhaustedError(message);
    }
    return absl::InternalError(message);
}

} // namespace

absl::StatusOr<FileHandle> MiniDfsFileSystem::create(std::string_view path,
                                                     const CreateOptions& options) {
    if (client_ == nullptr) {
        return absl::FailedPreconditionError("MiniDFS client is null");
    }
    auto stream = client_->create_immutable_output_stream(path, options.overwrite);
    if (stream.hasError()) {
        return to_absl_status(stream.error(), "create");
    }
    const auto handle = allocate_handle();
    std::lock_guard lock(mutex_);
    open_files_.emplace(handle,
                        std::make_unique<OpenFile>(std::in_place_type<minidfs::DfsOutputStream>,
                                                   std::move(stream).value()));
    return handle;
}

absl::StatusOr<FileHandle> MiniDfsFileSystem::open(std::string_view path) {
    if (client_ == nullptr) {
        return absl::FailedPreconditionError("MiniDFS client is null");
    }
    auto status = client_->stat(path);
    if (status.hasError()) {
        return to_absl_status(status.error(), "open");
    }
    const auto& file_status = status.value();
    if (file_status.is_dir ||
        file_status.file_append_mode != minidfs::FileAppendMode::kImmutableAfterComplete ||
        file_status.published_identity.inode_id == 0 ||
        !file_status.published_identity.checksum_valid) {
        return absl::FailedPreconditionError("MiniDFS file is not an immutable published file");
    }
    const auto handle = allocate_handle();
    std::lock_guard lock(mutex_);
    open_files_.emplace(
        handle,
        std::make_unique<OpenFile>(
            std::in_place_type<ReadFile>,
            ReadFile{.path = std::string(path), .identity = file_status.published_identity}));
    return handle;
}

absl::Status MiniDfsFileSystem::append(FileHandle handle, std::span<const std::byte> data) {
    std::lock_guard lock(mutex_);
    const auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return absl::FailedPreconditionError("file handle is closed or invalid");
    }
    auto* stream = std::get_if<minidfs::DfsOutputStream>(it->second.get());
    if (stream == nullptr) {
        return absl::FailedPreconditionError("file is not writable");
    }
    auto result = stream->write(data.data(), data.size());
    return result.hasError() ? to_absl_status(result.error(), "write") : absl::OkStatus();
}

absl::Status MiniDfsFileSystem::read_at(FileHandle handle,
                                        uint64_t offset,
                                        std::span<std::byte> destination) {
    std::lock_guard lock(mutex_);
    const auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return absl::FailedPreconditionError("file handle is closed or invalid");
    }
    auto* file = std::get_if<ReadFile>(it->second.get());
    if (file == nullptr) {
        return absl::FailedPreconditionError("file is not readable");
    }
    auto result = client_->read_exact(file->path, offset, destination.size(), file->identity);
    if (result.hasError()) {
        return to_absl_status(result.error(), "read_at");
    }
    if (result.value().size() != destination.size()) {
        return absl::DataLossError("MiniDFS returned a short exact read");
    }
    std::memcpy(destination.data(), result.value().data(), destination.size());
    return absl::OkStatus();
}

absl::StatusOr<uint64_t> MiniDfsFileSystem::size(FileHandle handle) {
    std::lock_guard lock(mutex_);
    const auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return absl::FailedPreconditionError("file handle is closed or invalid");
    }
    if (const auto* stream = std::get_if<minidfs::DfsOutputStream>(it->second.get())) {
        return stream->bytes_written();
    }
    return std::get<ReadFile>(*it->second).identity.length;
}

absl::Status MiniDfsFileSystem::close(FileHandle handle) {
    std::lock_guard lock(mutex_);
    const auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return absl::FailedPreconditionError("file handle is closed or invalid");
    }
    absl::Status status = absl::OkStatus();
    if (auto* stream = std::get_if<minidfs::DfsOutputStream>(it->second.get())) {
        auto result = stream->close();
        if (result.hasError()) {
            status = to_absl_status(result.error(), "close");
        }
    }
    open_files_.erase(it);
    return status;
}

absl::Status MiniDfsFileSystem::remove(std::string_view path) {
    if (client_ == nullptr) {
        return absl::FailedPreconditionError("MiniDFS client is null");
    }
    auto result = client_->rm(path, false);
    return result.hasError() ? to_absl_status(result.error(), "delete") : absl::OkStatus();
}

absl::Status MiniDfsFileSystem::rename(std::string_view source, std::string_view destination) {
    if (client_ == nullptr) {
        return absl::FailedPreconditionError("MiniDFS client is null");
    }
    auto result = client_->mv(source, destination);
    return result.hasError() ? to_absl_status(result.error(), "rename") : absl::OkStatus();
}

} // namespace pl::sstv2::io
