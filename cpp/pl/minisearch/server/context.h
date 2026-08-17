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

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cpp/pl/minisearch/server/collection_registry.h"

namespace pl::minisearch::server {

// Tenant registry hub (DESIGN.md §10.1): each tenant owns one
// CollectionRegistry at <data_dir>/<tenant> (persistence on) or purely
// in-memory (data_dir empty). Auth=off requests resolve to the "default"
// tenant. Registries are shared_ptr so an in-flight request keeps the
// registry (and its checkpoint thread) alive across a concurrent DropTenant.
class TenantContext {
public:
    TenantContext(std::string data_dir, std::string index_type);

    // 懒建：首次访问该租户时创建 registry、恢复持久化数据并启动 checkpoint
    // 线程。tenant 必须是合法资源名，否则返回 nullptr。
    std::shared_ptr<CollectionRegistry> Registry(const std::string& tenant);

    // 删除租户：registry 从表中摘除（引用计数归零时析构、停止 checkpoint
    // 线程），数据目录删除。注册表中没有该租户时返回 false。
    bool DropTenant(const std::string& tenant);

    struct TenantInfo {
        std::string name;
        size_t collections = 0;
        size_t active_documents = 0;
    };
    std::vector<TenantInfo> Tenants() const;

    const std::string& data_dir() const { return data_dir_; }

private:
    std::string data_dir_;
    std::string index_type_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<CollectionRegistry>> registries_;
};

} // namespace pl::minisearch::server
