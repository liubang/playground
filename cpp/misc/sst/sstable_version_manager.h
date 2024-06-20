// Copyright (c) 2024 The Authors. All rights reserved.
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

#pragma once

#include "cpp/misc/sst/sstable.h"

#include <map>
#include <memory>

namespace pl {

class SSTableVersion;
class SSTableVersionEdit;
class SSTableVersionManager;

using SSTableVersionRef = std::shared_ptr<SSTableVersion>;
using SSTableVersionEditRef = std::shared_ptr<SSTableVersionEdit>;

class SSTableVersion {
public:
    SSTableVersion() = default;

    void getAllSSTable(std::vector<SSTableRef>* sstable_list);

    std::vector<SSTableRef> listSSTables();

private:
    void copyFrom(const SSTableVersionRef& version);

    void addSSTable(const SSTableRef& sst);

    void delSSTable(SSTId id);

private:
    friend class SSTableVersionManager;

    std::map<SSTId, SSTableRef> sst_map_;
    // 按patch id倒序，patch id越大，数据越新
    std::map<PatchId, std::vector<SSTableRef>, std::greater<>> sst_patch_map_;
};

class SSTableVersionEdit {
public:
    void addSSTable(const SSTableRef& sstable);
    void delSSTable(const SSTId& sstid);

private:
    friend class SSTableVersionManager;

    std::vector<SSTableRef> add_sstables_; // 新增的sstable
    std::vector<SSTId> del_sstables_;      // 删除的sstable
};

class SSTableVersionManager {
public:
    void applyVersionEdit(const SSTableVersionEditRef& edit);

public:
    // 这里保留最新的version就行了
    SSTableVersionRef current_;
};

} // namespace pl
