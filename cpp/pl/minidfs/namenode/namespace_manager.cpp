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

#include "cpp/pl/minidfs/namenode/namespace_manager.h"

#include <fmt/format.h>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/common/time_util.h"

namespace pl::minidfs {

namespace {

pl::Result<pl::Void> validate_path(std::string_view path) {
    if (path.empty() || path[0] != '/') {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidPath),
                             "path must be absolute (start with '/')");
    }
    if (path.size() > kMaxPathLength) {
        return pl::makeError(
            static_cast<pl::status_code_t>(ErrorCode::kPathTooLong),
            fmt::format("path length {} exceeds max {}", path.size(), kMaxPathLength));
    }
    return pl::Void{};
}

} // namespace

NamespaceManager::NamespaceManager(MetadataStore* store) : store_(store) {}

// Path utilities
std::vector<std::string_view> NamespaceManager::split_path(std::string_view path) {
    std::vector<std::string_view> components;
    size_t start = 1; // skip leading '/'
    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        if (end > start) {
            components.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return components;
}

pl::Result<Inode> NamespaceManager::walk_path(const std::vector<std::string_view>& components) {
    // Start from root (inode_id = 1)
    auto current = store_->get_inode(kRootInodeId);
    if (current.hasError()) {
        return current;
    }

    for (const auto& name : components) {
        if (current.value().type != InodeType::kDirectory) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotDirectory),
                                 fmt::format("'{}' is not a directory", current.value().name));
        }
        auto child = store_->get_child(current.value().inode_id, name);
        if (child.hasError()) {
            return folly::makeUnexpected(child.error());
        }
        if (!child.value().has_value()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                                 fmt::format("path component '{}' not found", name));
        }
        current = std::move(child.value().value());
    }
    return current;
}

pl::Result<Inode> NamespaceManager::ensure_parent(const std::vector<std::string_view>& components,
                                                  std::string_view owner,
                                                  std::string_view group,
                                                  uint32_t permission) {
    // Walk and create intermediate directories as needed.
    auto current = store_->get_inode(kRootInodeId);
    if (current.hasError()) {
        return current;
    }

    // Walk all but the last component (the file/dir name itself).
    for (size_t i = 0; i + 1 < components.size(); ++i) {
        auto child = store_->get_child(current.value().inode_id, components[i]);
        if (child.hasError()) {
            return folly::makeUnexpected(child.error());
        }
        if (child.value().has_value()) {
            if (child.value()->type != InodeType::kDirectory) {
                return pl::makeError(
                    static_cast<pl::status_code_t>(ErrorCode::kNotDirectory),
                    fmt::format("'{}' exists but is not a directory", components[i]));
            }
            current = std::move(child.value().value());
        } else {
            // Create intermediate directory.
            auto id_result = store_->alloc_id("inode");
            if (id_result.hasError()) {
                return folly::makeUnexpected(id_result.error());
            }
            uint64_t ts = now_ms();
            Inode dir;
            dir.inode_id = id_result.value();
            dir.type = InodeType::kDirectory;
            dir.parent_id = current.value().inode_id;
            dir.name = std::string(components[i]);
            dir.owner = std::string(owner);
            dir.group = std::string(group);
            dir.permission = permission;
            dir.ctime_ms = ts;
            dir.mtime_ms = ts;
            dir.version = 1;

            auto create_res = store_->create_inode(dir);
            if (create_res.hasError()) {
                return folly::makeUnexpected(create_res.error());
            }
            current = dir;
        }
    }
    return current;
}

// Directory operations
pl::Result<Inode> NamespaceManager::mkdir(std::string_view path,
                                          std::string_view owner,
                                          std::string_view group,
                                          uint32_t permission,
                                          bool create_parent) {
    auto valid = validate_path(path);
    if (valid.hasError()) {
        return folly::makeUnexpected(valid.error());
    }

    auto components = split_path(path);
    if (components.empty()) {
        // Path is "/", root already exists.
        return store_->get_inode(kRootInodeId);
    }

    Inode parent;
    if (create_parent) {
        auto parent_result = ensure_parent(components, owner, group, permission);
        if (parent_result.hasError()) {
            return parent_result;
        }
        parent = std::move(parent_result.value());
    } else {
        // Walk to parent (all but last component).
        auto parent_components =
            std::vector<std::string_view>(components.begin(), components.end() - 1);
        auto parent_result = walk_path(parent_components);
        if (parent_result.hasError()) {
            return parent_result;
        }
        parent = std::move(parent_result.value());
    }

    // Check if target already exists.
    auto& dir_name = components.back();
    auto existing = store_->get_child(parent.inode_id, dir_name);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (existing.value().has_value()) {
        if (existing.value()->type == InodeType::kDirectory) {
            // Already exists as directory, idempotent.
            return existing.value().value();
        }
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                             fmt::format("'{}' already exists as a file", dir_name));
    }

    // Allocate ID and create.
    auto id_result = store_->alloc_id("inode");
    if (id_result.hasError()) {
        return folly::makeUnexpected(id_result.error());
    }

    uint64_t ts = now_ms();
    Inode dir;
    dir.inode_id = id_result.value();
    dir.type = InodeType::kDirectory;
    dir.parent_id = parent.inode_id;
    dir.name = std::string(dir_name);
    dir.owner = std::string(owner);
    dir.group = std::string(group);
    dir.permission = permission;
    dir.ctime_ms = ts;
    dir.mtime_ms = ts;
    dir.version = 1;

    auto create_res = store_->create_inode(dir);
    if (create_res.hasError()) {
        return folly::makeUnexpected(create_res.error());
    }
    return dir;
}

// File operations
pl::Result<Inode> NamespaceManager::create_file(std::string_view path,
                                                std::string_view owner,
                                                std::string_view group,
                                                uint32_t permission,
                                                uint32_t replication,
                                                uint64_t block_size,
                                                FileAppendMode file_append_mode) {
    auto valid = validate_path(path);
    if (valid.hasError()) {
        return folly::makeUnexpected(valid.error());
    }

    auto components = split_path(path);
    if (components.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidPath),
                             "cannot create file at root path");
    }

    // Resolve parent directory.
    auto parent_components =
        std::vector<std::string_view>(components.begin(), components.end() - 1);
    auto parent_result = walk_path(parent_components);
    if (parent_result.hasError()) {
        return parent_result;
    }
    auto& parent = parent_result.value();

    if (parent.type != InodeType::kDirectory) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotDirectory),
                             "parent is not a directory");
    }

    // Check if file already exists.
    auto& file_name = components.back();
    auto existing = store_->get_child(parent.inode_id, file_name);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (existing.value().has_value()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                             fmt::format("'{}' already exists", file_name));
    }

    // Allocate ID and create file inode.
    auto id_result = store_->alloc_id("inode");
    if (id_result.hasError()) {
        return folly::makeUnexpected(id_result.error());
    }

    uint64_t ts = now_ms();
    Inode file;
    file.inode_id = id_result.value();
    file.type = InodeType::kFile;
    file.parent_id = parent.inode_id;
    file.name = std::string(file_name);
    file.owner = std::string(owner);
    file.group = std::string(group);
    file.permission = permission;
    file.replication = replication;
    file.block_size = block_size;
    file.file_append_mode = file_append_mode;
    file.content_generation = 0;
    file.checksum = 0;
    file.state = FileState::kUnderConstruction;
    file.ctime_ms = ts;
    file.mtime_ms = ts;
    file.version = 1;

    auto create_res = store_->create_inode(file);
    if (create_res.hasError()) {
        return folly::makeUnexpected(create_res.error());
    }
    return file;
}

pl::Result<Inode> NamespaceManager::begin_append(std::string_view path) {
    auto inode = resolve_path(path);
    if (inode.hasError()) {
        return folly::makeUnexpected(inode.error());
    }
    if (inode.value().type != InodeType::kFile) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIsDirectory),
                             "cannot append to a directory");
    }
    if (inode.value().state != FileState::kNormal) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kFileUnderConstruction),
                             "file is already under construction");
    }
    if (inode.value().file_append_mode == FileAppendMode::kImmutableAfterComplete) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kPermissionDenied),
                             "append is disabled for immutable-after-complete files");
    }

    auto file = std::move(inode.value());
    file.state = FileState::kUnderConstruction;
    file.mtime_ms = now_ms();
    auto update = store_->update_inode(file);
    if (update.hasError()) {
        return folly::makeUnexpected(update.error());
    }
    return file;
}

pl::Result<Inode> NamespaceManager::remove(std::string_view path, bool recursive) {
    auto valid = validate_path(path);
    if (valid.hasError()) {
        return folly::makeUnexpected(valid.error());
    }

    auto components = split_path(path);
    if (components.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidPath),
                             "cannot remove root directory");
    }

    auto inode_result = walk_path(components);
    if (inode_result.hasError()) {
        return inode_result;
    }
    auto& inode = inode_result.value();

    if (inode.type == InodeType::kDirectory) {
        auto children = store_->list_children(inode.inode_id);
        if (children.hasError()) {
            return folly::makeUnexpected(children.error());
        }
        if (!children.value().empty() && !recursive) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kDirectoryNotEmpty),
                                 fmt::format("directory '{}' is not empty", inode.name));
        }
        // Recursive delete: remove all children using iterative DFS.
        if (recursive) {
            std::vector<uint64_t> delete_stack;
            // Collect all descendant inodes in post-order (children before parents).
            std::vector<uint64_t> to_delete;
            delete_stack.push_back(inode.inode_id);
            while (!delete_stack.empty()) {
                uint64_t cur = delete_stack.back();
                delete_stack.pop_back();
                auto cur_children = store_->list_children(cur);
                if (cur_children.hasError()) {
                    return folly::makeUnexpected(cur_children.error());
                }
                for (const auto& child : cur_children.value()) {
                    to_delete.push_back(child.inode_id);
                    if (child.type == InodeType::kDirectory) {
                        delete_stack.push_back(child.inode_id);
                    }
                }
            }
            // Delete in reverse order (deepest first) to respect FK constraints.
            for (auto it = to_delete.rbegin(); it != to_delete.rend(); ++it) {
                auto del = store_->delete_inode(*it);
                if (del.hasError()) {
                    return folly::makeUnexpected(del.error());
                }
            }
        }
    }

    auto del = store_->delete_inode(inode.inode_id);
    if (del.hasError()) {
        return folly::makeUnexpected(del.error());
    }
    return inode;
}

pl::Result<pl::Void> NamespaceManager::rename(std::string_view src, std::string_view dst) {
    auto src_valid = validate_path(src);
    if (src_valid.hasError()) {
        return src_valid;
    }
    auto dst_valid = validate_path(dst);
    if (dst_valid.hasError()) {
        return dst_valid;
    }
    if (src == dst) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                             "source and destination are identical");
    }

    auto src_components = split_path(src);
    auto dst_components = split_path(dst);

    if (src_components.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidPath),
                             "cannot rename root");
    }
    if (dst_components.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidPath),
                             "cannot rename to root");
    }

    // Resolve source.
    auto src_inode = walk_path(src_components);
    if (src_inode.hasError()) {
        return folly::makeUnexpected(src_inode.error());
    }

    // Resolve destination parent.
    auto dst_parent_components =
        std::vector<std::string_view>(dst_components.begin(), dst_components.end() - 1);
    auto dst_parent = walk_path(dst_parent_components);
    if (dst_parent.hasError()) {
        return folly::makeUnexpected(dst_parent.error());
    }

    if (src_inode.value().type == InodeType::kDirectory) {
        // Prevent moving a directory into itself or one of its descendants.
        uint64_t ancestor_id = dst_parent.value().inode_id;
        while (ancestor_id != 0) {
            if (ancestor_id == src_inode.value().inode_id) {
                return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                                     "cannot move a directory into itself or its descendant");
            }
            auto ancestor = store_->get_inode(ancestor_id);
            if (ancestor.hasError()) {
                return folly::makeUnexpected(ancestor.error());
            }
            ancestor_id = ancestor.value().parent_id;
        }
    }

    // Check destination doesn't already exist.
    auto& dst_name = dst_components.back();
    auto existing = store_->get_child(dst_parent.value().inode_id, dst_name);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (existing.value().has_value()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                             fmt::format("destination '{}' already exists", dst_name));
    }

    // Update inode: change parent_id and name.
    Inode updated = src_inode.value();
    updated.parent_id = dst_parent.value().inode_id;
    updated.name = std::string(dst_name);
    updated.mtime_ms = now_ms();

    return store_->update_inode(updated);
}

// Query operations
pl::Result<FileStatus> NamespaceManager::get_file_status(std::string_view path) {
    auto valid = validate_path(path);
    if (valid.hasError()) {
        return folly::makeUnexpected(valid.error());
    }

    auto components = split_path(path);
    auto inode_result = walk_path(components);
    if (inode_result.hasError()) {
        return folly::makeUnexpected(inode_result.error());
    }
    auto& inode = inode_result.value();

    FileStatus status;
    status.inode_id = inode.inode_id;
    status.path = std::string(path);
    status.is_dir = (inode.type == InodeType::kDirectory);
    status.length = inode.length;
    status.replication = inode.replication;
    status.block_size = inode.block_size;
    status.mtime_ms = inode.mtime_ms;
    status.owner = inode.owner;
    status.group = inode.group;
    status.permission = inode.permission;
    status.file_append_mode = inode.file_append_mode;
    status.published_identity = {
        .inode_id = inode.inode_id,
        .content_generation = inode.content_generation,
        .length = inode.length,
        .checksum = inode.checksum,
        .checksum_valid = inode.checksum_valid,
    };
    return status;
}

pl::Result<std::vector<FileStatus>> NamespaceManager::list_status(std::string_view path) {
    auto valid = validate_path(path);
    if (valid.hasError()) {
        return folly::makeUnexpected(valid.error());
    }

    auto components = split_path(path);
    auto inode_result = walk_path(components);
    if (inode_result.hasError()) {
        return folly::makeUnexpected(inode_result.error());
    }
    auto& inode = inode_result.value();

    if (inode.type != InodeType::kDirectory) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotDirectory),
                             "path is not a directory");
    }

    auto children = store_->list_children(inode.inode_id);
    if (children.hasError()) {
        return folly::makeUnexpected(children.error());
    }

    std::vector<FileStatus> result;
    result.reserve(children.value().size());
    for (const auto& child : children.value()) {
        FileStatus fs;
        fs.inode_id = child.inode_id;
        fs.path = (path == "/") ? ("/" + child.name) : (std::string(path) + "/" + child.name);
        fs.is_dir = (child.type == InodeType::kDirectory);
        fs.length = child.length;
        fs.replication = child.replication;
        fs.block_size = child.block_size;
        fs.mtime_ms = child.mtime_ms;
        fs.owner = child.owner;
        fs.group = child.group;
        fs.permission = child.permission;
        fs.file_append_mode = child.file_append_mode;
        fs.published_identity = {
            .inode_id = child.inode_id,
            .content_generation = child.content_generation,
            .length = child.length,
            .checksum = child.checksum,
            .checksum_valid = child.checksum_valid,
        };
        result.push_back(std::move(fs));
    }
    return result;
}

pl::Result<Inode> NamespaceManager::resolve_path(std::string_view path) {
    auto valid = validate_path(path);
    if (valid.hasError()) {
        return folly::makeUnexpected(valid.error());
    }
    return walk_path(split_path(path));
}

pl::Result<pl::Void> NamespaceManager::complete_file(uint64_t inode_id, uint64_t final_length) {
    auto identity = complete_file_publish_identity(inode_id, final_length, std::nullopt);
    if (identity.hasError()) {
        return folly::makeUnexpected(identity.error());
    }
    return pl::Void{};
}

pl::Result<FileIdentity> NamespaceManager::complete_file_publish_identity(
    uint64_t inode_id, uint64_t final_length, std::optional<uint32_t> checksum) {
    auto inode_result = store_->get_inode(inode_id);
    if (inode_result.hasError()) {
        return folly::makeUnexpected(inode_result.error());
    }
    auto& inode = inode_result.value();

    if (inode.type != InodeType::kFile) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIsDirectory),
                             "cannot complete a directory");
    }

    const bool already_completed = inode.state == FileState::kNormal;
    if (already_completed) {
        if (inode.length != final_length) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                                 "complete replay length mismatch");
        }
        if (!checksum.has_value()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                                 "complete replay requires expected checksum");
        }
        if (!inode.checksum_valid || inode.checksum != checksum.value()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kChecksumMismatch),
                                 "complete replay checksum does not match published identity");
        }
    }

    if (!already_completed) {
        inode.state = FileState::kNormal;
        inode.length = final_length;
        inode.content_generation += 1;
        inode.checksum = checksum.value_or(0);
        inode.checksum_valid = checksum.has_value();
        inode.mtime_ms = now_ms();
        auto update = store_->update_inode(inode);
        if (update.hasError()) {
            return folly::makeUnexpected(update.error());
        }
    }

    FileIdentity identity;
    identity.inode_id = inode.inode_id;
    identity.content_generation = inode.content_generation;
    identity.length = inode.length;
    identity.checksum = inode.checksum;
    identity.checksum_valid = inode.checksum_valid;
    return identity;
}

pl::Result<FileIdentity> NamespaceManager::get_file_identity(uint64_t inode_id) {
    auto inode_result = store_->get_inode(inode_id);
    if (inode_result.hasError()) {
        return folly::makeUnexpected(inode_result.error());
    }
    const auto& inode = inode_result.value();
    if (inode.type != InodeType::kFile) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIsDirectory),
                             "inode is not a file");
    }

    FileIdentity identity;
    identity.inode_id = inode.inode_id;
    identity.content_generation = inode.content_generation;
    identity.length = inode.length;
    identity.checksum = inode.checksum;
    identity.checksum_valid = inode.checksum_valid;
    return identity;
}

pl::Result<pl::Void> NamespaceManager::set_file_length(uint64_t inode_id,
                                                      uint64_t length,
                                                      std::optional<uint32_t> checksum) {
    auto inode_result = store_->get_inode(inode_id);
    if (inode_result.hasError()) {
        return folly::makeUnexpected(inode_result.error());
    }
    auto& inode = inode_result.value();
    if (inode.type != InodeType::kFile) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIsDirectory),
                             "cannot truncate a directory");
    }
    if (inode.state != FileState::kNormal) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kFileUnderConstruction),
                             "cannot truncate a file under construction");
    }
    if (inode.file_append_mode == FileAppendMode::kImmutableAfterComplete) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kPermissionDenied),
                             "truncate is disabled for immutable-after-complete files");
    }
    if (length > inode.length) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                             "truncate length exceeds current file length");
    }
    if (length == inode.length) {
        return pl::Void{};
    }

    inode.length = length;
    inode.content_generation += 1;
    inode.checksum = checksum.value_or(0);
    inode.checksum_valid = checksum.has_value();
    inode.mtime_ms = now_ms();
    return store_->update_inode(inode);
}

pl::Result<pl::Void> NamespaceManager::set_replication(uint64_t inode_id, uint32_t replication) {
    auto inode_result = store_->get_inode(inode_id);
    if (inode_result.hasError()) {
        return folly::makeUnexpected(inode_result.error());
    }
    auto& inode = inode_result.value();
    if (inode.type != InodeType::kFile) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIsDirectory),
                             "cannot set replication on a directory");
    }
    if (inode.state != FileState::kNormal) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kFileUnderConstruction),
                             "cannot set replication on a file under construction");
    }
    inode.replication = replication;
    inode.mtime_ms = now_ms();
    return store_->update_inode(inode);
}

} // namespace pl::minidfs
