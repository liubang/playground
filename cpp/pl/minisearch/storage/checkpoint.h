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
// Created: 2026/08/17

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cpp/pl/minisearch/core/collection.h"
#include "cpp/pl/minisearch/core/schema.h"
#include "cpp/pl/minisearch/faiss_index.h"
#include "cpp/pl/minisearch/proto/minisearch.pb.h"

namespace pl::minisearch::storage {

// 检查点存储（DESIGN.md §5.1/§5.2）。目录布局：
//   <root>/<collection>/manifest.json          # CheckpointManifest JSON，原子写
//   <root>/<collection>/checkpoint.<seq>.faiss # 向量索引（无向量字段则缺省）
//   <root>/<collection>/checkpoint.<seq>.docs  # 文档快照，4B LE 长度前缀的 proto Document
// 滚动保留最近 2 个 seq；tombstone 不落盘，恢复后等效物理清除。
class CheckpointStore {
public:
    // root 通常为 <data_dir>/<tenant>。
    explicit CheckpointStore(std::string root);

    // 写入一个 collection 的检查点。index 为空表示该 collection 无向量字段。
    // 成功后调用方应执行 collection->MarkCheckpointed()。
    bool Save(const std::string& collection,
              const proto::CollectionSpec& spec,
              const std::string& index_type,
              const core::Collection& docs,
              const FaissIndex* index);

    struct Loaded {
        proto::CollectionSpec spec;
        std::string index_type;
        std::vector<core::Document> documents;
        std::string faiss_path; // 空 = 无向量索引；调用方执行 FaissIndex::load
    };

    std::optional<Loaded> Load(const std::string& collection) const;

    std::vector<std::string> ListCollections() const;

    // 删除 collection 目录。
    bool Drop(const std::string& collection);

    const std::string& root() const { return root_; }

private:
    std::string root_;
};

} // namespace pl::minisearch::storage
