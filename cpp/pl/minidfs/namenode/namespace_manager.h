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
// Created: 2026/05/10 17:30

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// NamespaceManager — manages the directory tree (inode namespace).
//
// Responsibilities:
//   - Path resolution (path string -> inode chain)
//   - mkdir, create file, delete, rename, list, stat
//   - Enforces naming rules and directory consistency
//   - Allocates inode IDs via MetadataStore

class NamespaceManager {
public:
    explicit NamespaceManager(MetadataStore* store);
    ~NamespaceManager() = default;

    NamespaceManager(const NamespaceManager&) = delete;
    NamespaceManager& operator=(const NamespaceManager&) = delete;

    // Directory operations

    /// Create a directory at the given path. Creates intermediate dirs if create_parent=true.
    pl::Result<Inode> mkdir(std::string_view path,
                            std::string_view owner,
                            std::string_view group,
                            uint32_t permission,
                            bool create_parent = false);

    // File operations

    /// Create a new file. The file starts in UnderConstruction state.
    /// Returns the created inode. Caller must hold a lease to write blocks.
    pl::Result<Inode> create_file(std::string_view path,
                                  std::string_view owner,
                                  std::string_view group,
                                  uint32_t permission,
                                  uint32_t replication,
                                  uint64_t block_size);

    /// Reopen an existing file for append.
    pl::Result<Inode> begin_append(std::string_view path);

    /// Delete a file or empty directory. Returns the deleted inode.
    pl::Result<Inode> remove(std::string_view path, bool recursive = false);

    /// Rename/move a file or directory.
    pl::Result<pl::Void> rename(std::string_view src, std::string_view dst);

    // Query operations

    /// Get file/directory status by path.
    pl::Result<FileStatus> get_file_status(std::string_view path);

    /// List directory contents.
    pl::Result<std::vector<FileStatus>> list_status(std::string_view path);

    /// Resolve a path to its inode. Returns NotFound if path does not exist.
    pl::Result<Inode> resolve_path(std::string_view path);

    // Internal helpers (public for testability)

    /// Complete a file (transition from UnderConstruction to Normal).
    pl::Result<pl::Void> complete_file(uint64_t inode_id, uint64_t final_length);

    /// Update the length of an existing closed file after block truncation.
    pl::Result<pl::Void> set_file_length(uint64_t inode_id, uint64_t length);

    /// Update the desired replication factor recorded on an existing closed file.
    pl::Result<pl::Void> set_replication(uint64_t inode_id, uint32_t replication);

private:
    /// Split an absolute path into components.
    static std::vector<std::string_view> split_path(std::string_view path);

    /// Walk the path from root, returning the final inode.
    pl::Result<Inode> walk_path(const std::vector<std::string_view>& components);

    /// Get or create parent directories for a path. Returns the parent inode.
    pl::Result<Inode> ensure_parent(const std::vector<std::string_view>& components,
                                    std::string_view owner,
                                    std::string_view group,
                                    uint32_t permission);

    MetadataStore* store_;
};

} // namespace pl::minidfs
