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

#include "cpp/pl/sstv2/io/local_filesystem.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace pl::sstv2::io {
namespace {

absl::Status errno_status(std::string_view operation, std::string_view path, int error) {
    const std::string message = absl::StrCat(operation, " ", path, ": ", std::strerror(error));
    switch (error) {
        case EEXIST:
            return absl::AlreadyExistsError(message);
        case ENOENT:
            return absl::NotFoundError(message);
        case EACCES:
        case EPERM:
            return absl::PermissionDeniedError(message);
        case ENOSPC:
            return absl::ResourceExhaustedError(message);
        case EINVAL:
        case EISDIR:
        case ENOTDIR:
            return absl::InvalidArgumentError(message);
        default:
            return absl::InternalError(message);
    }
}

absl::Status invalid_handle() {
    return absl::FailedPreconditionError(
        "file handle is closed, invalid, or belongs to another filesystem");
}

} // namespace

LocalFileSystem::~LocalFileSystem() {
    std::lock_guard lock(mutex_);
    for (const auto& [handle, file] : open_files_) {
        static_cast<void>(handle);
        std::lock_guard file_lock(file->mutex);
        static_cast<void>(::close(file->descriptor));
        file->descriptor = -1;
    }
}

absl::StatusOr<std::shared_ptr<LocalFileSystem::OpenFile>> LocalFileSystem::get_open_file(
    FileHandle handle) const {
    std::lock_guard lock(mutex_);
    const auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return invalid_handle();
    }
    return it->second;
}

absl::StatusOr<FileHandle> LocalFileSystem::create(std::string_view path,
                                                   const CreateOptions& options) {
    const std::string owned_path(path);
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    flags |= options.overwrite ? O_TRUNC : O_EXCL;
    const int descriptor = ::open(owned_path.c_str(), flags, 0644);
    if (descriptor < 0) {
        return errno_status("create", path, errno);
    }
    const auto handle = allocate_handle();
    std::lock_guard lock(mutex_);
    auto file = std::make_shared<OpenFile>();
    file->descriptor = descriptor;
    file->writable = true;
    open_files_.emplace(handle, std::move(file));
    return handle;
}

absl::StatusOr<FileHandle> LocalFileSystem::open(std::string_view path) {
    const std::string owned_path(path);
    const int descriptor = ::open(owned_path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return errno_status("open", path, errno);
    }
    const auto handle = allocate_handle();
    std::lock_guard lock(mutex_);
    auto file = std::make_shared<OpenFile>();
    file->descriptor = descriptor;
    file->writable = false;
    open_files_.emplace(handle, std::move(file));
    return handle;
}

absl::Status LocalFileSystem::append(FileHandle handle, std::span<const std::byte> data) {
    auto file = get_open_file(handle);
    if (!file.ok()) {
        return file.status();
    }
    std::lock_guard file_lock((*file)->mutex);
    if ((*file)->descriptor < 0) {
        return invalid_handle();
    }
    if (!(*file)->writable) {
        return absl::FailedPreconditionError("file is not writable");
    }
    size_t written = 0;
    while (written < data.size()) {
        const size_t remaining = data.size() - written;
        const size_t request =
            std::min(remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::write((*file)->descriptor, data.data() + written, request);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_status("append", "file", errno);
        }
        if (result == 0) {
            return absl::InternalError("append made no progress");
        }
        written += static_cast<size_t>(result);
    }
    return absl::OkStatus();
}

absl::Status LocalFileSystem::read_at(FileHandle handle,
                                      uint64_t offset,
                                      std::span<std::byte> destination) {
    auto file = get_open_file(handle);
    if (!file.ok()) {
        return file.status();
    }
    std::lock_guard file_lock((*file)->mutex);
    if ((*file)->descriptor < 0) {
        return invalid_handle();
    }
    if ((*file)->writable) {
        return absl::FailedPreconditionError("file is not readable");
    }
    constexpr auto kMaxOffset = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
    const uint64_t length = destination.size();
    if (offset > kMaxOffset || (length > 0 && length - 1 > kMaxOffset - offset)) {
        return absl::OutOfRangeError("read range exceeds platform limits");
    }
    size_t read = 0;
    while (read < destination.size()) {
        const size_t remaining = destination.size() - read;
        const size_t request =
            std::min(remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const auto position = static_cast<off_t>(offset + read);
        const ssize_t count =
            ::pread((*file)->descriptor, destination.data() + read, request, position);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_status("pread", "file", errno);
        }
        if (count == 0) {
            return absl::OutOfRangeError(
                "read reached end of file before reading requested length");
        }
        read += static_cast<size_t>(count);
    }
    return absl::OkStatus();
}

absl::StatusOr<uint64_t> LocalFileSystem::size(FileHandle handle) {
    auto file = get_open_file(handle);
    if (!file.ok()) {
        return file.status();
    }
    std::lock_guard file_lock((*file)->mutex);
    if ((*file)->descriptor < 0) {
        return invalid_handle();
    }
    struct stat info{};
    if (::fstat((*file)->descriptor, &info) != 0) {
        return errno_status("fstat", "file", errno);
    }
    if (info.st_size < 0) {
        return absl::DataLossError("local file has negative size");
    }
    return static_cast<uint64_t>(info.st_size);
}

absl::Status LocalFileSystem::close(FileHandle handle) {
    std::shared_ptr<OpenFile> file;
    {
        std::lock_guard lock(mutex_);
        const auto it = open_files_.find(handle);
        if (it == open_files_.end()) {
            return invalid_handle();
        }
        file = std::move(it->second);
        open_files_.erase(it);
    }
    std::lock_guard file_lock(file->mutex);
    const int descriptor = std::exchange(file->descriptor, -1);
    if (::close(descriptor) != 0) {
        return errno_status("close", "file", errno);
    }
    return absl::OkStatus();
}

absl::Status LocalFileSystem::remove(std::string_view path) {
    const std::string owned_path(path);
    if (::unlink(owned_path.c_str()) != 0) {
        return errno_status("remove", path, errno);
    }
    return absl::OkStatus();
}

absl::Status LocalFileSystem::rename(std::string_view source, std::string_view destination) {
    const std::string owned_source(source);
    const std::string owned_destination(destination);
    struct stat info{};
    if (::lstat(owned_destination.c_str(), &info) == 0) {
        return absl::AlreadyExistsError("rename destination already exists");
    }
    if (errno != ENOENT) {
        return errno_status("stat rename destination", destination, errno);
    }
    if (::rename(owned_source.c_str(), owned_destination.c_str()) != 0) {
        return errno_status("rename", source, errno);
    }
    return absl::OkStatus();
}

} // namespace pl::sstv2::io
