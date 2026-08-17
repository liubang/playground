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

#include "cpp/pl/minisearch/core/id_mapper.h"

namespace pl::minisearch::core {

int64_t DocIdMapper::Lookup(const std::string& id) const {
    auto it = id_to_doc_.find(id);
    return it == id_to_doc_.end() ? -1 : it->second;
}

int64_t DocIdMapper::Assign(const std::string& id, std::optional<int64_t>* tombstoned) {
    *tombstoned = std::nullopt;
    const int64_t fresh = next_++;
    auto [it, inserted] = id_to_doc_.emplace(id, fresh);
    if (!inserted) {
        *tombstoned = it->second;
        tombstones_.insert(it->second);
        it->second = fresh;
    }
    return fresh;
}

bool DocIdMapper::Remove(const std::string& id) {
    auto it = id_to_doc_.find(id);
    if (it == id_to_doc_.end()) {
        return false;
    }
    tombstones_.insert(it->second);
    id_to_doc_.erase(it);
    return true;
}

} // namespace pl::minisearch::core
