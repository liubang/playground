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

#include "cpp/pl/minisearch/server/context.h"

#include "cpp/pl/minisearch/core/schema.h"

namespace pl::minisearch::server {

TenantContext::TenantContext(std::string data_dir, std::string index_type)
    : data_dir_(std::move(data_dir)), index_type_(std::move(index_type)) {}

std::shared_ptr<CollectionRegistry> TenantContext::Registry(const std::string& tenant) {
    if (!core::IsValidResourceName(tenant)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto it = registries_.find(tenant);
    if (it != registries_.end()) {
        return it->second;
    }
    std::unique_ptr<storage::CheckpointStore> store;
    if (!data_dir_.empty()) {
        store = std::make_unique<storage::CheckpointStore>(data_dir_ + "/" + tenant);
    }
    auto registry = std::make_shared<CollectionRegistry>(std::move(store), index_type_);
    if (!data_dir_.empty()) {
        registry->LoadFromDisk();
        registry->StartCheckpointLoop(std::make_shared<std::atomic<bool>>(false));
    }
    registries_[tenant] = registry;
    return registry;
}

bool TenantContext::DropTenant(const std::string& tenant) {
    std::lock_guard<std::mutex> lock(mu_);
    // registry 摘除后由引用计数决定析构时机：in-flight 请求持有的
    // shared_ptr 使其（含 checkpoint 线程与 store）存活到请求结束。
    const bool had = registries_.erase(tenant) > 0;
    if (!had || data_dir_.empty()) {
        return had;
    }
    storage::CheckpointStore store(data_dir_ + "/" + tenant);
    for (const auto& name : store.ListCollections()) {
        store.Drop(name);
    }
    return true;
}

std::vector<TenantContext::TenantInfo> TenantContext::Tenants() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<TenantInfo> out;
    out.reserve(registries_.size());
    // 锁序固定为 context -> registry（registry 不会回调 context），无死锁。
    for (const auto& [name, registry] : registries_) {
        const auto stats = registry->GetStats();
        out.push_back({name, stats.collections, stats.active_documents});
    }
    return out;
}

} // namespace pl::minisearch::server
