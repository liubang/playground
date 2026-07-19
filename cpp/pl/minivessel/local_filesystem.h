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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cpp/pl/minivessel/active_log_storage.h"
#include "cpp/pl/minivessel/filesystem.h"
#include "cpp/pl/sstv2/io/local_filesystem.h"

namespace pl::minivessel {

// POSIX backend for development and deterministic tests. It is thread-safe. An advisory flock
// serializes live writers across instances/processes; persisted epochs and durable boundaries make
// restart recovery fail closed. Lease recovery is intentionally not advertised because POSIX flock
// cannot fence a live-but-expired process.
class LocalFileSystem final : public ObjectMetadataBackend, public ActiveLogStorage {
public:
    LocalFileSystem();
    ~LocalFileSystem() override;

    LocalFileSystem(const LocalFileSystem&) = delete;
    LocalFileSystem& operator=(const LocalFileSystem&) = delete;

    [[nodiscard]] ObjectStorageCapabilities capabilities() const noexcept override {
        return ObjectStorageCapabilities(
            static_cast<uint32_t>(ObjectStorageFeature::kImmutableObjects));
    }
    [[nodiscard]] ActiveLogCapabilities active_log_capabilities() const noexcept override {
        return ActiveLogCapabilities(static_cast<uint32_t>(ActiveLogFeature::kDurableAppend) |
                                     static_cast<uint32_t>(ActiveLogFeature::kWriterFencing) |
                                     static_cast<uint32_t>(ActiveLogFeature::kDurableTail));
    }

    [[nodiscard]] std::shared_ptr<sstv2::io::FileSystem> object_filesystem() const override;
    [[nodiscard]] absl::StatusOr<WriterSession> acquire_writer(
        const AcquireWriterRequest& request) override;
    [[nodiscard]] absl::StatusOr<WriterSession> renew_writer(WriterHandle handle) override;
    [[nodiscard]] absl::Status append(WriterHandle handle,
                                      const AppendOptions& options,
                                      std::span<const std::byte> data) override;
    [[nodiscard]] absl::StatusOr<SyncResult> sync(WriterHandle handle) override;
    [[nodiscard]] absl::StatusOr<ByteOffset> durable_size(std::string_view path) override;
    [[nodiscard]] absl::Status read_durable(std::string_view path,
                                            ByteOffset offset,
                                            std::span<std::byte> destination) override;
    [[nodiscard]] absl::Status seal(WriterHandle handle) override;
    [[nodiscard]] absl::Status release_writer(WriterHandle handle) override;

public:
    // Implementation records are exposed as incomplete types only so out-of-line codec helpers can
    // remain allocation-free; they are not part of the storage protocol or construction API.
    struct OpenWriter;
    struct PersistedMetadata;

private:
    [[nodiscard]] WriterHandle allocate_writer_handle() noexcept;
    [[nodiscard]] absl::StatusOr<std::shared_ptr<OpenWriter>> get_writer(WriterHandle handle) const;
    [[nodiscard]] static absl::StatusOr<PersistedMetadata> read_metadata(std::string_view path,
                                                                         bool wal_created);
    [[nodiscard]] static absl::Status persist_metadata(std::string_view path,
                                                       const PersistedMetadata& metadata);
    [[nodiscard]] absl::Status close_writer(WriterHandle handle, bool seal);

    std::shared_ptr<sstv2::io::LocalFileSystem> object_filesystem_;
    std::atomic<uint64_t> next_handle_{1};
    mutable std::mutex mutex_;
    std::unordered_map<WriterHandle, std::shared_ptr<OpenWriter>> writers_;
};
} // namespace pl::minivessel
