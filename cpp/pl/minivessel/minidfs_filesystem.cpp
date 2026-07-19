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

#include "cpp/pl/minivessel/minidfs_filesystem.h"

#include <utility>

#include "cpp/pl/minidfs/client/dfs_client.h"

namespace pl::minivessel {

MiniDfsFileSystem::MiniDfsFileSystem(std::shared_ptr<minidfs::DfsClient> client)
    : client_(std::move(client)),
      object_filesystem_(std::make_shared<sstv2::io::MiniDfsFileSystem>(client_)) {}

std::shared_ptr<sstv2::io::FileSystem> MiniDfsFileSystem::object_filesystem() const {
    return object_filesystem_;
}

} // namespace pl::minivessel
