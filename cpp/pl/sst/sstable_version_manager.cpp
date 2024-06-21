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

#include "cpp/pl/sst/sstable_version_manager.h"

namespace pl {

std::vector<SSTableRef> SSTableVersion::listSSTables() {
    std::vector<SSTableRef> ret;
    auto iter = sst_patch_map_.begin();
    for (; iter != sst_patch_map_.end(); ++iter) {
        ret.insert(ret.end(), iter->second.begin(), iter->second.end());
    }
    return ret;
}

void SSTableVersion::addSSTable(const SSTableRef& sst) {
    auto sst_id = sst->sstId();
    assert(sst_map_.find(sst_id) == sst_map_.end());
    sst_map_[sst_id] = sst;
    sst_patch_map_[sst->patchId()].push_back(sst);
}

void SSTableVersion::delSSTable(SSTId id) {
    auto iter = sst_map_.find(id);
    assert(iter != sst_map_.end());
    auto sst = iter->second;
    sst_map_.erase(iter);
    auto& list = sst_patch_map_[sst->patchId()];
    assert(!list.empty());
    for (auto it = list.begin(); it != list.end(); ++it) {
        if ((*it)->sstId() == sst->sstId()) {
            list.erase(it);
            break;
        }
    }
    if (list.empty()) {
        sst_patch_map_.erase(sst->patchId());
    }
}

void SSTableVersion::copyFrom(const SSTableVersionRef& version) {
    sst_map_ = version->sst_map_;
    sst_patch_map_ = version->sst_patch_map_;
}

void SSTableVersionEdit::addSSTable(const SSTableRef& sstable) { add_sstables_.push_back(sstable); }

void SSTableVersionEdit::delSSTable(const SSTId& sstid) { del_sstables_.push_back(sstid); }

void SSTableVersionManager::applyVersionEdit(const SSTableVersionEditRef& edit) {
    SSTableVersionRef new_version = std::make_shared<SSTableVersion>();
    new_version->copyFrom(current_);

    for (const auto& id : edit->del_sstables_) {
        new_version->delSSTable(id);
    }

    for (const auto& sst : edit->add_sstables_) {
        new_version->addSSTable(sst);
    }

    // TODO(liubang): 是否需要保证线程安全
    current_ = new_version;
}

} // namespace pl
