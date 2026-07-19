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
// Created: 2026/07/18 00:36

#include "cpp/pl/minitable/core/slice_store.h"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "cpp/pl/sstv2/merge/merge_iterator.h"
#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/row.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {
namespace {

std::atomic<uint64_t> g_next_store_incarnation{1};

absl::Status InjectFault(const FlushFaultInjector& injector, FlushFaultPoint point) {
    return injector ? injector(point) : absl::OkStatus();
}

class LocalityGroupCursor final : public sstv2::merge::ForwardCursor {
public:
    explicit LocalityGroupCursor(std::vector<sstv2::merge::Source> sources)
        : merge_(std::move(sources)) {}

    absl::Status seek_to_first() override { return merge_.seek_to_first(); }
    absl::Status seek(std::string_view encoded_key) override { return merge_.seek(encoded_key); }

    absl::Status next() override {
        if (!merge_.valid()) {
            return absl::FailedPreconditionError("locality group cursor is not valid");
        }
        std::string current_key;
        try {
            current_key.assign(merge_.key());
        } catch (const std::bad_alloc&) {
            return absl::ResourceExhaustedError("cursor duplicate-key allocation failed");
        }
        do {
            auto status = merge_.next();
            if (!status.ok()) {
                return status;
            }
        } while (merge_.valid() && merge_.key() == current_key);
        return absl::OkStatus();
    }

    bool valid() const override { return merge_.valid(); }
    std::string_view key() const override { return merge_.key(); }
    std::string_view value() const override { return merge_.value(); }

private:
    sstv2::merge::MergeIterator merge_;
};

class FailedBuildCleanup final {
public:
    FailedBuildCleanup(std::shared_ptr<sstv2::io::FileSystem> filesystem,
                       std::string key_path,
                       std::string value_path)
        : filesystem_(std::move(filesystem)),
          key_path_(std::move(key_path)),
          value_path_(std::move(value_path)) {}
    ~FailedBuildCleanup() {
        if (filesystem_ != nullptr && !released_) {
            (void)filesystem_->remove(key_path_);
            (void)filesystem_->remove(value_path_);
        }
    }
    void release() noexcept { released_ = true; }

private:
    std::shared_ptr<sstv2::io::FileSystem> filesystem_;
    std::string key_path_;
    std::string value_path_;
    bool released_ = false;
};

class PendingSinks final {
public:
    PendingSinks(std::shared_ptr<sstv2::io::FileSystem> filesystem,
                 std::string key_path,
                 std::string value_path,
                 sstv2::io::FileHandle key,
                 sstv2::io::FileHandle value)
        : filesystem_(std::move(filesystem)),
          key_path_(std::move(key_path)),
          value_path_(std::move(value_path)),
          key_(key),
          value_(value) {}
    ~PendingSinks() {
        if (filesystem_ == nullptr || released_) {
            return;
        }
        CloseAndRemove(key_path_, key_);
        CloseAndRemove(value_path_, value_);
    }
    void release() noexcept { released_ = true; }

private:
    void CloseAndRemove(std::string_view path, sstv2::io::FileHandle handle) noexcept {
        if (!handle.valid()) {
            return;
        }
        auto identity = filesystem_->close(handle);
        if (identity.ok()) {
            (void)filesystem_->remove(path, *identity);
        } else {
            (void)filesystem_->remove(path);
        }
    }

    std::shared_ptr<sstv2::io::FileSystem> filesystem_;
    std::string key_path_;
    std::string value_path_;
    sstv2::io::FileHandle key_;
    sstv2::io::FileHandle value_;
    bool released_ = false;
};

absl::StatusOr<sstv2::types::Schema::ConstRef> FlushSchema() {
    try {
        auto schema = sstv2::types::SchemaBuilder()
                          .add_column("minitable_encoded_key", sstv2::types::DataType::kBinary)
                          .build();
        if (!schema.has_value()) {
            return absl::InternalError("failed to construct minitable flush schema");
        }
        return std::make_shared<const sstv2::types::Schema>(std::move(*schema));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("flush schema allocation failed");
    }
}

} // namespace

absl::StatusOr<std::unique_ptr<SliceStore>> SliceStore::Create(
    std::map<uint32_t, MemTableOptions> locality_group_options, SliceStorePersistence persistence) {
    if (locality_group_options.empty()) {
        return absl::InvalidArgumentError("SliceStore requires at least one locality group");
    }
    const bool persistent =
        persistence.filesystem != nullptr || !persistence.manifest_directory.empty();
    if (persistent &&
        (persistence.filesystem == nullptr || persistence.manifest_directory.empty())) {
        return absl::InvalidArgumentError("incomplete SliceStore persistence options");
    }
    if (!IsValidComparatorDomain(persistence.comparator_domain) ||
        persistence.timestamp_domain_epoch == 0) {
        return absl::InvalidArgumentError("invalid SliceStore persistence domain");
    }
    if (persistent && locality_group_options.size() != 1) {
        return absl::UnimplementedError(
            "persistent reopen currently requires the Phase 1A single locality group model");
    }

    try {
        auto manifest = std::make_shared<ManifestRef>();
        manifest->comparator_domain = persistence.comparator_domain;
        manifest->timestamp_domain_epoch = persistence.timestamp_domain_epoch;
        for (const auto& [locality_group_id, options] : locality_group_options) {
            static_cast<void>(options);
            manifest->locality_groups.emplace(locality_group_id, LocalityGroupManifest{});
        }
        auto version = std::make_shared<SliceVersion>();
        version->generation = 1;
        version->manifest = manifest;
        for (const auto& [locality_group_id, options] : locality_group_options) {
            if (locality_group_id == 0) {
                return absl::InvalidArgumentError("locality group ID zero is reserved");
            }
            auto memtable = MemTable::Create(options);
            if (!memtable.ok()) {
                return memtable.status();
            }
            version->locality_groups.emplace(
                locality_group_id,
                LocalityGroupReadState{.active = std::move(*memtable), .options = options});
        }
        auto initial =
            std::make_shared<PublishedReadState>(PublishedReadState{.version = std::move(version),
                                                                    .visible_applied_index = 0,
                                                                    .timestamp_high_watermark = 0,
                                                                    .last_commit_physical_ms = 0});
        PersistedManifest persisted;
        if (persistent) {
            auto result = PersistManifest(persistence.filesystem,
                                          persistence.manifest_directory + "/manifest-1.mtmf",
                                          *manifest);
            if (!result.ok()) {
                return result.status();
            }
            persisted = std::move(*result);
        }
        const uint64_t incarnation =
            g_next_store_incarnation.fetch_add(1, std::memory_order_relaxed);
        if (incarnation == 0) {
            return absl::OutOfRangeError("SliceStore incarnation is exhausted");
        }
        return std::unique_ptr<SliceStore>(new SliceStore(
            incarnation, std::move(initial), std::move(persistence), std::move(persisted)));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SliceStore allocation failed");
    }
}

absl::StatusOr<std::unique_ptr<SliceStore>> SliceStore::Reopen(SliceStoreRecovery recovery) {
    if (recovery.locality_groups.size() != 1 || recovery.persistence.filesystem == nullptr ||
        recovery.persistence.manifest_directory.empty()) {
        return absl::InvalidArgumentError(
            "SliceStore recovery requires exactly one persistent locality group");
    }
    auto manifest = LoadManifest(recovery.persistence.filesystem, recovery.manifest);
    if (!manifest.ok()) {
        return manifest.status();
    }
    if ((*manifest)->comparator_domain != recovery.persistence.comparator_domain ||
        (*manifest)->timestamp_domain_epoch != recovery.persistence.timestamp_domain_epoch) {
        return absl::FailedPreconditionError("recovery comparator or timestamp domain mismatch");
    }
    if ((*manifest)->locality_groups.size() != recovery.locality_groups.size()) {
        return absl::FailedPreconditionError("recovery locality groups do not match manifest");
    }

    try {
        auto version = std::make_shared<SliceVersion>();
        version->generation = (*manifest)->generation;
        version->manifest = *manifest;
        uint64_t visible_applied_index = 0;
        for (const auto& [locality_group_id, options] : recovery.locality_groups) {
            const auto manifest_group = (*manifest)->locality_groups.find(locality_group_id);
            if (locality_group_id == 0 || manifest_group == (*manifest)->locality_groups.end()) {
                return absl::FailedPreconditionError("recovery locality group is missing");
            }
            auto active = MemTable::Create(options);
            if (!active.ok()) {
                return active.status();
            }
            LocalityGroupReadState group{.active = std::move(*active), .options = options};
            group.ssts.reserve(manifest_group->second.ssts.size());
            for (const auto& sst : manifest_group->second.ssts) {
                auto source = SstReadSource::Open(recovery.persistence.filesystem, sst.identity);
                if (!source.ok()) {
                    return source.status();
                }
                group.ssts.push_back(std::move(*source));
            }
            visible_applied_index =
                std::max(visible_applied_index, manifest_group->second.flushed_applied_index);
            version->locality_groups.emplace(locality_group_id, std::move(group));
        }
        auto initial = std::make_shared<PublishedReadState>(
            PublishedReadState{.version = std::move(version),
                               .visible_applied_index = visible_applied_index,
                               .timestamp_high_watermark = (*manifest)->timestamp_high_watermark,
                               .last_commit_physical_ms = (*manifest)->last_commit_physical_ms});
        const uint64_t incarnation =
            g_next_store_incarnation.fetch_add(1, std::memory_order_relaxed);
        if (incarnation == 0) {
            return absl::OutOfRangeError("SliceStore incarnation is exhausted");
        }
        auto store = std::unique_ptr<SliceStore>(new SliceStore(incarnation,
                                                                std::move(initial),
                                                                std::move(recovery.persistence),
                                                                std::move(recovery.manifest)));
        for (auto& candidate : recovery.orphan_candidates) {
            const bool live = std::ranges::any_of(
                (*manifest)->locality_groups, [&candidate](const auto& locality_group) {
                    return std::ranges::any_of(
                        locality_group.second.ssts,
                        [&candidate](const auto& sst) { return sst.identity == candidate; });
                });
            if (live) {
                return absl::FailedPreconditionError(
                    "recovery orphan candidate is referenced by the live Manifest");
            }
            if (std::ranges::find(store->orphans_, candidate) == store->orphans_.end()) {
                store->orphans_.push_back(std::move(candidate));
            }
        }
        if (!(*manifest)->installed_edits.empty()) {
            const uint64_t last_edit_id = (*manifest)->installed_edits.back().edit_id;
            if (last_edit_id == std::numeric_limits<uint64_t>::max()) {
                return absl::OutOfRangeError("recovered immutable identity is exhausted");
            }
            store->next_immutable_id_ = last_edit_id + 1;
        }
        return store;
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SliceStore recovery allocation failed");
    }
}

absl::Status SliceStore::apply(std::span<const LocalityGroupPatch> patches, uint64_t apply_index) {
    if (patches.empty()) {
        return absl::InvalidArgumentError("SliceStore patch list is empty");
    }
    const auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    return apply_committed(patches,
                           apply_index,
                           std::max(current->timestamp_high_watermark, apply_index),
                           current->last_commit_physical_ms);
}

absl::Status SliceStore::apply_committed(std::span<const LocalityGroupPatch> patches,
                                         uint64_t apply_index,
                                         uint64_t timestamp_high_watermark,
                                         uint64_t commit_physical_ms) {
    std::lock_guard apply_lock(apply_mutex_);
    const auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    if (apply_index <= current->visible_applied_index) {
        return absl::InvalidArgumentError("SliceStore apply index must advance");
    }
    if (timestamp_high_watermark < current->timestamp_high_watermark ||
        commit_physical_ms < current->last_commit_physical_ms) {
        return absl::InvalidArgumentError("committed timestamp metadata must not regress");
    }

    std::map<uint32_t, std::span<const MemTableMutation>> canonical;
    try {
        for (const auto& patch : patches) {
            if (patch.locality_group_id == 0 || patch.mutations.empty()) {
                return absl::InvalidArgumentError("invalid locality group patch");
            }
            if (!current->version->locality_groups.contains(patch.locality_group_id)) {
                return absl::NotFoundError("locality group does not exist");
            }
            if (!canonical.emplace(patch.locality_group_id, patch.mutations).second) {
                return absl::InvalidArgumentError("duplicate locality group patch");
            }
        }
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Slice patch canonicalization allocation failed");
    }

    std::shared_ptr<const PublishedReadState> next;
    try {
        next = std::make_shared<PublishedReadState>(
            PublishedReadState{.version = current->version,
                               .visible_applied_index = apply_index,
                               .timestamp_high_watermark = timestamp_high_watermark,
                               .last_commit_physical_ms = commit_physical_ms});
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Slice read-state allocation failed");
    }

    std::vector<MemTable::PreparedBatch> prepared;
    try {
        prepared.reserve(canonical.size());
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Slice prepare list allocation failed");
    }
    for (const auto& [locality_group_id, mutations] : canonical) {
        auto batch = current->version->locality_groups.at(locality_group_id)
                         .active->prepare_batch(mutations, apply_index);
        if (!batch.ok()) {
            return batch.status();
        }
        prepared.push_back(std::move(*batch));
    }

    for (auto& batch : prepared) {
        batch.publish();
    }
    std::atomic_store_explicit(&published_, std::move(next), std::memory_order_release);
    return absl::OkStatus();
}

absl::Status SliceStore::freeze_locality_group(uint32_t locality_group_id) {
    std::lock_guard apply_lock(apply_mutex_);
    const auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    const auto found = current->version->locality_groups.find(locality_group_id);
    if (found == current->version->locality_groups.end()) {
        return absl::NotFoundError("locality group does not exist");
    }
    if (found->second.immutable != nullptr) {
        return absl::FailedPreconditionError("locality group already has an immutable MemTable");
    }
    if (found->second.active->size() == 0) {
        return absl::FailedPreconditionError("cannot freeze an empty active MemTable");
    }
    if (current->version->generation == std::numeric_limits<uint64_t>::max() ||
        next_immutable_id_ == 0) {
        return absl::OutOfRangeError("Slice structure identity is exhausted");
    }

    auto new_active = MemTable::Create(found->second.options);
    if (!new_active.ok()) {
        return new_active.status();
    }

    std::shared_ptr<const PublishedReadState> next;
    try {
        auto version = std::make_shared<SliceVersion>(*current->version);
        auto& locality_group = version->locality_groups.at(locality_group_id);
        locality_group.immutable = locality_group.active;
        locality_group.immutable_id = next_immutable_id_;
        locality_group.immutable_fence_index = current->visible_applied_index;
        locality_group.immutable_timestamp_high_watermark = current->timestamp_high_watermark;
        locality_group.immutable_last_commit_physical_ms = current->last_commit_physical_ms;
        locality_group.active = std::move(*new_active);
        version->generation = current->version->generation + 1;
        next = std::make_shared<PublishedReadState>(
            PublishedReadState{.version = std::move(version),
                               .visible_applied_index = current->visible_applied_index,
                               .timestamp_high_watermark = current->timestamp_high_watermark,
                               .last_commit_physical_ms = current->last_commit_physical_ms});
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Slice freeze read-state allocation failed");
    }

    auto status = found->second.active->freeze();
    if (!status.ok()) {
        return status;
    }
    ++next_immutable_id_;
    std::atomic_store_explicit(&published_, std::move(next), std::memory_order_release);
    return absl::OkStatus();
}

absl::StatusOr<FlushToken> SliceStore::begin_flush(uint32_t locality_group_id) const {
    std::lock_guard lock(apply_mutex_);
    const auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    const auto found = current->version->locality_groups.find(locality_group_id);
    if (found == current->version->locality_groups.end()) {
        return absl::NotFoundError("locality group does not exist");
    }
    if (found->second.immutable == nullptr || found->second.immutable_id == 0) {
        return absl::FailedPreconditionError("locality group has no immutable MemTable");
    }
    return FlushToken(store_incarnation_,
                      locality_group_id,
                      found->second.immutable_id,
                      found->second.immutable_fence_index,
                      found->second.immutable_timestamp_high_watermark,
                      found->second.immutable_last_commit_physical_ms,
                      current->version->manifest->comparator_domain,
                      found->second.immutable);
}

absl::StatusOr<FinalizedFlush> SliceStore::build_flush_sst(const FlushToken& token,
                                                           FlushSstOptions options) {
    if (token.immutable_ == nullptr || token.immutable_id_ == 0 || options.filesystem == nullptr ||
        options.key_path.empty() || options.value_path.empty() ||
        options.key_path == options.value_path) {
        return absl::InvalidArgumentError("invalid flush token or SST options");
    }
    auto status = InjectFault(options.fault_injector, FlushFaultPoint::kBeforeKeyCreate);
    if (!status.ok()) {
        return status;
    }
    auto schema = FlushSchema();
    if (!schema.ok()) {
        return schema.status();
    }
    FailedBuildCleanup failed_build(options.filesystem, options.key_path, options.value_path);
    auto key = options.filesystem->create(options.key_path);
    if (!key.ok()) {
        return key.status();
    }
    status = InjectFault(options.fault_injector, FlushFaultPoint::kAfterKeyCreate);
    if (!status.ok()) {
        (void)options.filesystem->close(*key);
        return status;
    }
    auto value = options.filesystem->create(options.value_path);
    if (!value.ok()) {
        (void)options.filesystem->close(*key);
        return value.status();
    }

    status = InjectFault(options.fault_injector, FlushFaultPoint::kAfterValueCreate);
    if (!status.ok()) {
        (void)options.filesystem->close(*key);
        (void)options.filesystem->close(*value);
        return status;
    }
    PendingSinks pending(options.filesystem, options.key_path, options.value_path, *key, *value);
    options.builder_options.value_file_name = options.value_path;
    options.builder_options.configuration.sst_format_version = kMinitableSstFormatVersion;
    options.builder_options.configuration.key_format_version =
        token.comparator_domain_.key_format_version;
    options.builder_options.configuration.row_key_schema_fingerprint =
        token.comparator_domain_.row_key_schema_fingerprint;
    options.builder_options.configuration.comparator_domain_fingerprint =
        token.comparator_domain_.fingerprint;
    options.builder_options.configuration.checksum_algorithm = kCrc32cChecksumAlgorithm;
    std::unique_ptr<sstv2::file::Builder> builder;
    try {
        builder = std::make_unique<sstv2::file::Builder>(
            *schema,
            sstv2::file::Sinks{.filesystem = options.filesystem, .key = *key, .value = *value},
            std::move(options.builder_options));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SST builder allocation failed");
    }
    pending.release();
    auto cursor = token.immutable_->new_cursor(token.fence_index_);
    status = cursor->seek_to_first();
    if (!status.ok()) {
        return status;
    }
    while (cursor->valid()) {
        try {
            auto row = sstv2::types::Row::create(
                sstv2::types::RowKey::from_columns(
                    {sstv2::types::Value::make<sstv2::types::DataType::kBinary>(cursor->key())}),
                {.version = {.major = 0, .minor = 0}, .op_type = sstv2::types::OpType::kPut},
                sstv2::types::Value::make<sstv2::types::DataType::kBinary>(cursor->value()));
            status = builder->add(row);
        } catch (const std::bad_alloc&) {
            status = absl::ResourceExhaustedError("flush row allocation failed");
        }
        if (!status.ok()) {
            return status;
        }
        status = cursor->next();
        if (!status.ok()) {
            return status;
        }
    }

    status = InjectFault(options.fault_injector, FlushFaultPoint::kBeforeSstFinalize);
    if (!status.ok()) {
        return status;
    }
    auto result = builder->finish_result();
    if (!result.ok()) {
        return result.status();
    }
    status = InjectFault(options.fault_injector, FlushFaultPoint::kAfterSstFinalize);
    if (!status.ok()) {
        return status;
    }
    SstIdentity identity{
        .key_path = std::move(options.key_path),
        .value_path = std::move(options.value_path),
        .key_file = result->key_file,
        .value_file = result->value_file,
        .row_count = result->row_count,
        .sst_format_version = result->sst_format_version,
        .comparator_domain = token.comparator_domain_,
        .checksum_algorithm = result->checksum_algorithm,
    };
    auto source = SstReadSource::Open(options.filesystem, std::move(identity));
    if (!source.ok()) {
        return source.status();
    }
    failed_build.release();
    return FinalizedFlush(token.store_incarnation_,
                          token.locality_group_id_,
                          token.immutable_id_,
                          token.fence_index_,
                          std::move(*source));
}

absl::Status SliceStore::install_flush(const FlushToken& token, const FinalizedFlush& flush) {
    if (token.store_incarnation_ != store_incarnation_ || token.immutable_ == nullptr ||
        flush.source_ == nullptr || flush.store_incarnation_ != token.store_incarnation_ ||
        flush.locality_group_id_ != token.locality_group_id_ ||
        flush.immutable_id_ != token.immutable_id_ || flush.fence_index_ != token.fence_index_) {
        return absl::InvalidArgumentError("flush output provenance does not match token");
    }

    std::lock_guard lock(apply_mutex_);
    const auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    const auto found = current->version->locality_groups.find(token.locality_group_id_);
    if (found == current->version->locality_groups.end()) {
        return absl::NotFoundError("locality group does not exist");
    }
    if (found->second.last_installed_immutable_id == token.immutable_id_) {
        return !found->second.ssts.empty() &&
                       found->second.ssts.front()->identity() == flush.source_->identity()
                   ? absl::OkStatus()
                   : absl::AlreadyExistsError("immutable was installed with a different SST");
    }
    if (found->second.immutable == nullptr || found->second.immutable_id != token.immutable_id_ ||
        found->second.immutable_fence_index != token.fence_index_ ||
        found->second.immutable != token.immutable_) {
        return absl::AbortedError("stale immutable flush token");
    }
    if (current->version->generation == std::numeric_limits<uint64_t>::max() ||
        current->version->manifest == nullptr) {
        return absl::OutOfRangeError("SliceVersion generation is exhausted or has no manifest");
    }

    ManifestEdit edit{
        .edit_id = token.immutable_id_,
        .locality_group_id = token.locality_group_id_,
        .parent_generation = current->version->manifest->generation,
        .new_generation = current->version->manifest->generation + 1,
        .immutable_id = token.immutable_id_,
        .immutable_fence_index = token.fence_index_,
        .timestamp_high_watermark = token.timestamp_high_watermark_,
        .last_commit_physical_ms = token.last_commit_physical_ms_,
        .outputs = {flush.source_->identity()},
    };
    auto manifest = ApplyManifestEdit(*current->version->manifest, edit);
    if (!manifest.ok()) {
        try {
            orphans_.push_back(flush.source_->identity());
        } catch (const std::bad_alloc&) {
            return absl::ResourceExhaustedError("failed to record rejected flush orphan");
        }
        return manifest.status();
    }

    PersistedManifest persisted;
    if (persistence_.filesystem != nullptr) {
        auto fault =
            InjectFault(persistence_.fault_injector, FlushFaultPoint::kBeforeManifestPersist);
        if (!fault.ok()) {
            try {
                orphans_.push_back(flush.source_->identity());
            } catch (const std::bad_alloc&) {
                return absl::ResourceExhaustedError("failed to record injected flush orphan");
            }
            return fault;
        }
        auto result = PersistManifest(persistence_.filesystem,
                                      persistence_.manifest_directory + "/manifest-" +
                                          std::to_string((*manifest)->generation) + ".mtmf",
                                      **manifest);
        if (!result.ok()) {
            try {
                orphans_.push_back(flush.source_->identity());
            } catch (const std::bad_alloc&) {
                return absl::ResourceExhaustedError("failed to record uncommitted flush orphan");
            }
            return result.status();
        }
        persisted = std::move(*result);
        fault = InjectFault(persistence_.fault_injector, FlushFaultPoint::kAfterManifestPersist);
        if (!fault.ok()) {
            (void)persistence_.filesystem->remove(persisted.path, persisted.identity);
            try {
                orphans_.push_back(flush.source_->identity());
            } catch (const std::bad_alloc&) {
                return absl::ResourceExhaustedError("failed to record injected flush orphan");
            }
            return fault;
        }
    }

    std::shared_ptr<const PublishedReadState> next;
    try {
        auto version = std::make_shared<SliceVersion>(*current->version);
        auto& locality_group = version->locality_groups.at(token.locality_group_id_);
        locality_group.ssts.insert(locality_group.ssts.begin(), flush.source_);
        locality_group.immutable.reset();
        locality_group.last_installed_immutable_id = token.immutable_id_;
        locality_group.immutable_id = 0;
        locality_group.immutable_fence_index = 0;
        locality_group.immutable_timestamp_high_watermark = 0;
        locality_group.immutable_last_commit_physical_ms = 0;
        version->generation = current->version->generation + 1;
        version->manifest = std::move(*manifest);
        next = std::make_shared<PublishedReadState>(
            PublishedReadState{.version = std::move(version),
                               .visible_applied_index = current->visible_applied_index,
                               .timestamp_high_watermark = current->timestamp_high_watermark,
                               .last_commit_physical_ms = current->last_commit_physical_ms});
    } catch (const std::bad_alloc&) {
        if (!persisted.path.empty()) {
            (void)persistence_.filesystem->remove(persisted.path, persisted.identity);
        }
        try {
            orphans_.push_back(flush.source_->identity());
        } catch (const std::bad_alloc&) {
            return absl::ResourceExhaustedError(
                "flush install failed and its orphan could not be recorded");
        }
        return absl::ResourceExhaustedError("flush install read-state allocation failed");
    }
    auto fault = InjectFault(persistence_.fault_injector, FlushFaultPoint::kBeforeReadStatePublish);
    if (!fault.ok()) {
        if (!persisted.path.empty()) {
            (void)persistence_.filesystem->remove(persisted.path, persisted.identity);
        }
        try {
            orphans_.push_back(flush.source_->identity());
        } catch (const std::bad_alloc&) {
            return absl::ResourceExhaustedError("failed to record injected flush orphan");
        }
        return fault;
    }
    std::atomic_store_explicit(&published_, std::move(next), std::memory_order_release);
    if (!persisted.path.empty()) {
        // Generation-named Manifest objects are immutable. The predecessor remains
        // available as the last crash-safe authority until a future GC proves that
        // no snapshot/recovery reference can still name it.
        if (!persisted_manifest_.path.empty()) {
            obsolete_manifests_.push_back(persisted_manifest_);
        }
        persisted_manifest_ = std::move(persisted);
    }
    return absl::OkStatus();
}

uint64_t SliceStore::minimum_flushed_applied_index() const {
    const auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    uint64_t minimum = std::numeric_limits<uint64_t>::max();
    for (const auto& [group_id, group] : current->version->manifest->locality_groups) {
        static_cast<void>(group_id);
        minimum = std::min(minimum, group.flushed_applied_index);
    }
    return minimum == std::numeric_limits<uint64_t>::max() ? 0 : minimum;
}

SliceReadView SliceStore::read_view() const {
    return SliceReadView(std::atomic_load_explicit(&published_, std::memory_order_acquire));
}

size_t SliceStore::orphan_count() const {
    std::lock_guard lock(apply_mutex_);
    return orphans_.size();
}

absl::Status SliceStore::collect_manifests_before(uint64_t minimum_generation_to_keep) {
    std::lock_guard lock(apply_mutex_);
    if (persistence_.filesystem == nullptr) {
        return absl::FailedPreconditionError("Manifest GC requires a persistent filesystem");
    }
    std::vector<PersistedManifest> remaining;
    absl::Status first_error;
    for (const auto& manifest : obsolete_manifests_) {
        if (manifest.generation >= minimum_generation_to_keep) {
            remaining.push_back(manifest);
            continue;
        }
        const auto status = persistence_.filesystem->remove(manifest.path, manifest.identity);
        if (!status.ok()) {
            remaining.push_back(manifest);
            if (first_error.ok()) {
                first_error = status;
            }
        }
    }
    obsolete_manifests_ = std::move(remaining);
    return first_error;
}

absl::Status SliceStore::collect_orphans() {
    std::lock_guard lock(apply_mutex_);
    if (orphans_.empty()) {
        return absl::OkStatus();
    }
    if (persistence_.filesystem == nullptr) {
        return absl::FailedPreconditionError("orphan GC requires a persistent filesystem");
    }
    std::vector<SstIdentity> remaining;
    try {
        remaining.reserve(orphans_.size());
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("orphan GC allocation failed");
    }
    absl::Status first_error;
    for (const auto& orphan : orphans_) {
        const auto key_status = persistence_.filesystem->remove(orphan.key_path, orphan.key_file);
        const auto value_status =
            persistence_.filesystem->remove(orphan.value_path, orphan.value_file);
        if (!key_status.ok() || !value_status.ok()) {
            remaining.push_back(orphan);
            if (first_error.ok()) {
                first_error = !key_status.ok() ? key_status : value_status;
            }
        }
    }
    orphans_ = std::move(remaining);
    return first_error;
}

bool SliceReadView::has_immutable(uint32_t locality_group_id) const {
    const auto it = state_->version->locality_groups.find(locality_group_id);
    return it != state_->version->locality_groups.end() && it->second.immutable != nullptr;
}

size_t SliceReadView::sst_count(uint32_t locality_group_id) const {
    const auto it = state_->version->locality_groups.find(locality_group_id);
    return it == state_->version->locality_groups.end() ? 0 : it->second.ssts.size();
}

absl::StatusOr<std::unique_ptr<sstv2::merge::ForwardCursor>> SliceReadView::new_cursor(
    uint32_t locality_group_id) const {
    const auto it = state_->version->locality_groups.find(locality_group_id);
    if (it == state_->version->locality_groups.end()) {
        return absl::NotFoundError("locality group does not exist in read view");
    }

    try {
        std::vector<sstv2::merge::Source> sources;
        sources.reserve(1 + (it->second.immutable != nullptr ? 1 : 0) + it->second.ssts.size());
        sources.push_back({.cursor = it->second.active->new_cursor(state_->visible_applied_index),
                           .priority = 0});
        uint32_t priority = 1;
        if (it->second.immutable != nullptr) {
            sources.push_back(
                {.cursor = it->second.immutable->new_cursor(state_->visible_applied_index),
                 .priority = priority++});
        }
        for (const auto& sst : it->second.ssts) {
            auto cursor = sst->new_cursor();
            if (!cursor.ok()) {
                return cursor.status();
            }
            sources.push_back({.cursor = std::move(*cursor), .priority = priority++});
        }
        return std::make_unique<LocalityGroupCursor>(std::move(sources));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("locality group cursor allocation failed");
    }
}

} // namespace pl::minitable
