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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minitable/core/manifest.h"
#include "cpp/pl/minitable/core/sst_read_source.h"
#include "cpp/pl/minitable/memtable/memtable.h"
#include "cpp/pl/sstv2/file/sstable.h"
#include "cpp/pl/sstv2/io/filesystem.h"

namespace pl::minitable {

struct LocalityGroupPatch {
    uint32_t locality_group_id = 0;
    std::span<const MemTableMutation> mutations;
};

struct LocalityGroupReadState {
    std::shared_ptr<MemTable> active;
    std::shared_ptr<MemTable> immutable;
    uint64_t immutable_id = 0;
    uint64_t immutable_fence_index = 0;
    uint64_t immutable_timestamp_high_watermark = 0;
    uint64_t immutable_last_commit_physical_ms = 0;
    uint64_t last_installed_immutable_id = 0;
    // Newest first. Lower source priority therefore wins on duplicate keys.
    std::vector<std::shared_ptr<const SstReadSource>> ssts;
    MemTableOptions options;
};

struct SliceVersion {
    uint64_t generation = 0;
    std::shared_ptr<const ManifestRef> manifest;
    std::map<uint32_t, LocalityGroupReadState> locality_groups;
};

struct PublishedReadState {
    std::shared_ptr<const SliceVersion> version;
    uint64_t visible_applied_index = 0;
    uint64_t timestamp_high_watermark = 0;
    uint64_t last_commit_physical_ms = 0;
};

class FlushToken final {
public:
    FlushToken(const FlushToken&) = delete;
    FlushToken& operator=(const FlushToken&) = delete;
    FlushToken(FlushToken&&) noexcept = default;
    FlushToken& operator=(FlushToken&&) noexcept = default;

    [[nodiscard]] uint32_t locality_group_id() const { return locality_group_id_; }
    [[nodiscard]] uint64_t immutable_id() const { return immutable_id_; }
    [[nodiscard]] uint64_t fence_index() const { return fence_index_; }
    [[nodiscard]] uint64_t timestamp_high_watermark() const { return timestamp_high_watermark_; }
    [[nodiscard]] uint64_t last_commit_physical_ms() const { return last_commit_physical_ms_; }

private:
    friend class SliceStore;
    FlushToken(uint64_t store_incarnation,
               uint32_t locality_group_id,
               uint64_t immutable_id,
               uint64_t fence_index,
               uint64_t timestamp_high_watermark,
               uint64_t last_commit_physical_ms,
               ComparatorDomain comparator_domain,
               std::shared_ptr<MemTable> immutable)
        : store_incarnation_(store_incarnation),
          locality_group_id_(locality_group_id),
          immutable_id_(immutable_id),
          fence_index_(fence_index),
          timestamp_high_watermark_(timestamp_high_watermark),
          last_commit_physical_ms_(last_commit_physical_ms),
          comparator_domain_(comparator_domain),
          immutable_(std::move(immutable)) {}

    uint64_t store_incarnation_ = 0;
    uint32_t locality_group_id_ = 0;
    uint64_t immutable_id_ = 0;
    uint64_t fence_index_ = 0;
    uint64_t timestamp_high_watermark_ = 0;
    uint64_t last_commit_physical_ms_ = 0;
    ComparatorDomain comparator_domain_;
    std::shared_ptr<MemTable> immutable_;
};

enum class FlushFaultPoint : uint8_t {
    kBeforeKeyCreate,
    kAfterKeyCreate,
    kAfterValueCreate,
    kBeforeSstFinalize,
    kAfterSstFinalize,
    kBeforeManifestPersist,
    kAfterManifestPersist,
    kBeforeReadStatePublish,
};
using FlushFaultInjector = std::function<absl::Status(FlushFaultPoint)>;

struct SliceStorePersistence {
    std::shared_ptr<sstv2::io::FileSystem> filesystem;
    std::string manifest_directory;
    ComparatorDomain comparator_domain = kMinitableComparatorDomain;
    uint64_t timestamp_domain_epoch = 1;
    FlushFaultInjector fault_injector;
};

struct SliceStoreRecovery {
    std::map<uint32_t, MemTableOptions> locality_groups;
    SliceStorePersistence persistence;
    PersistedManifest manifest;
    // Finalized outputs discovered by an operation ledger/directory owner but absent
    // from the authoritative Manifest. They are never opened and are GC candidates.
    std::vector<SstIdentity> orphan_candidates;
};

struct FlushSstOptions {
    std::shared_ptr<sstv2::io::FileSystem> filesystem;
    std::string key_path;
    std::string value_path;
    sstv2::file::BuilderOptions builder_options;
    FlushFaultInjector fault_injector;
};

class FinalizedFlush final {
public:
    [[nodiscard]] const SstIdentity& identity() const { return source_->identity(); }

private:
    friend class SliceStore;
    FinalizedFlush(uint64_t store_incarnation,
                   uint32_t locality_group_id,
                   uint64_t immutable_id,
                   uint64_t fence_index,
                   std::shared_ptr<const SstReadSource> source)
        : store_incarnation_(store_incarnation),
          locality_group_id_(locality_group_id),
          immutable_id_(immutable_id),
          fence_index_(fence_index),
          source_(std::move(source)) {}

    uint64_t store_incarnation_ = 0;
    uint32_t locality_group_id_ = 0;
    uint64_t immutable_id_ = 0;
    uint64_t fence_index_ = 0;
    std::shared_ptr<const SstReadSource> source_;
};

class SliceReadView final {
public:
    [[nodiscard]] uint64_t generation() const { return state_->version->generation; }
    [[nodiscard]] uint64_t visible_applied_index() const { return state_->visible_applied_index; }
    [[nodiscard]] bool has_immutable(uint32_t locality_group_id) const;
    [[nodiscard]] size_t sst_count(uint32_t locality_group_id) const;
    [[nodiscard]] const ManifestRef& manifest() const { return *state_->version->manifest; }
    [[nodiscard]] absl::StatusOr<std::unique_ptr<sstv2::merge::ForwardCursor>> new_cursor(
        uint32_t locality_group_id) const;

private:
    friend class SliceStore;
    explicit SliceReadView(std::shared_ptr<const PublishedReadState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<const PublishedReadState> state_;
};

// In-process Slice storage coordinator. Writers serialize through apply_mutex_;
// readers atomically pin one immutable PublishedReadState containing both the
// structural SliceVersion and its compatible visibility watermark.
class SliceStore final {
public:
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<SliceStore>> Create(
        std::map<uint32_t, MemTableOptions> locality_groups,
        SliceStorePersistence persistence = {});
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<SliceStore>> Reopen(
        SliceStoreRecovery recovery);

    [[nodiscard]] absl::Status apply(std::span<const LocalityGroupPatch> patches,
                                     uint64_t apply_index);
    [[nodiscard]] absl::Status apply_committed(std::span<const LocalityGroupPatch> patches,
                                               uint64_t apply_index,
                                               uint64_t timestamp_high_watermark,
                                               uint64_t commit_physical_ms);
    [[nodiscard]] absl::Status freeze_locality_group(uint32_t locality_group_id);

    // Flush build performs all IO outside apply_mutex_. install_flush validates
    // the per-immutable fence against the latest SliceVersion and atomically
    // installs the SST while retiring exactly that immutable generation.
    [[nodiscard]] absl::StatusOr<FlushToken> begin_flush(uint32_t locality_group_id) const;
    [[nodiscard]] static absl::StatusOr<FinalizedFlush> build_flush_sst(const FlushToken& token,
                                                                        FlushSstOptions options);
    [[nodiscard]] absl::Status install_flush(const FlushToken& token, const FinalizedFlush& flush);

    [[nodiscard]] SliceReadView read_view() const;
    [[nodiscard]] const PersistedManifest& persisted_manifest() const noexcept {
        return persisted_manifest_;
    }
    [[nodiscard]] size_t orphan_count() const;
    [[nodiscard]] absl::Status collect_orphans();
    [[nodiscard]] absl::Status collect_manifests_before(uint64_t minimum_generation_to_keep);
    [[nodiscard]] uint64_t visible_applied_index() const {
        return std::atomic_load_explicit(&published_, std::memory_order_acquire)
            ->visible_applied_index;
    }
    [[nodiscard]] uint64_t timestamp_high_watermark() const {
        return std::atomic_load_explicit(&published_, std::memory_order_acquire)
            ->timestamp_high_watermark;
    }
    [[nodiscard]] uint64_t last_commit_physical_ms() const {
        return std::atomic_load_explicit(&published_, std::memory_order_acquire)
            ->last_commit_physical_ms;
    }
    [[nodiscard]] uint64_t minimum_flushed_applied_index() const;
    [[nodiscard]] bool can_snapshot_at(uint64_t snapshot_index) const {
        return snapshot_index != 0 && snapshot_index <= minimum_flushed_applied_index();
    }
    [[nodiscard]] const SliceStorePersistence& persistence() const noexcept { return persistence_; }
    [[nodiscard]] uint64_t generation() const {
        return std::atomic_load_explicit(&published_, std::memory_order_acquire)
            ->version->generation;
    }

private:
    SliceStore(uint64_t store_incarnation,
               std::shared_ptr<const PublishedReadState> initial,
               SliceStorePersistence persistence,
               PersistedManifest persisted_manifest)
        : store_incarnation_(store_incarnation),
          published_(std::move(initial)),
          persistence_(std::move(persistence)),
          persisted_manifest_(std::move(persisted_manifest)) {}

    mutable std::mutex apply_mutex_;
    uint64_t store_incarnation_ = 0;
    uint64_t next_immutable_id_ = 1;
    std::shared_ptr<const PublishedReadState> published_;
    SliceStorePersistence persistence_;
    PersistedManifest persisted_manifest_;
    std::vector<SstIdentity> orphans_;
    std::vector<PersistedManifest> obsolete_manifests_;
};

} // namespace pl::minitable
