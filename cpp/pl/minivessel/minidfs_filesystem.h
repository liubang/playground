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

#include "cpp/pl/minivessel/filesystem.h"
#include "cpp/pl/sstv2/io/minidfs_filesystem.h"

namespace pl::minidfs {
class DfsClient;
} // namespace pl::minidfs

namespace pl::minivessel {

// MiniDFS adapter for immutable checkpoint and sealed-segment objects only. Election and active-log
// authority are external services.
class MiniDfsFileSystem final : public ObjectMetadataBackend {
public:
    explicit MiniDfsFileSystem(std::shared_ptr<minidfs::DfsClient> client);

    [[nodiscard]] ObjectStorageCapabilities capabilities() const noexcept override {
        return ObjectStorageCapabilities(
            static_cast<uint32_t>(ObjectStorageFeature::kImmutableObjects));
    }

    [[nodiscard]] std::shared_ptr<sstv2::io::FileSystem> object_filesystem() const override;

private:
    std::shared_ptr<minidfs::DfsClient> client_;
    std::shared_ptr<sstv2::io::MiniDfsFileSystem> object_filesystem_;
};
} // namespace pl::minivessel
