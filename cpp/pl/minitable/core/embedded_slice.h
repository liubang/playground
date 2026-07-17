// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/key.h"
#include "cpp/pl/minitable/core/slice_store.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {

struct CellMutation {
    RecordTarget target;
    sstv2::types::OpType op_type = sstv2::types::OpType::kPut;
    std::optional<sstv2::types::Value> value;
};

struct VisibleCell {
    CellRef ref;
    Timestamp commit_ts;
    uint32_t mutation_seq = 0;
    sstv2::types::Value value;
};

struct VisibleRow {
    std::vector<sstv2::types::Value> row_key;
    // Canonical (column_family_id, qualifier-kind, qualifier-value) order.
    std::vector<VisibleCell> cells;
};

struct EmbeddedSliceOptions {
    uint32_t locality_group_id = 1;
    uint64_t timestamp_domain_epoch = 1;
    MemTableOptions memtable;
    SliceStorePersistence persistence;
};

struct EmbeddedSliceRecovery {
    EmbeddedSliceOptions options;
    PersistedManifest manifest;
};

// Phase-1A single-Slice/single-LG MVCC data plane. It deliberately allocates
// Slice-local timestamps in-process; callers must flush before treating writes
// as durable because WAL/Raft integration belongs to Phase 1B.
class EmbeddedSlice final {
public:
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<EmbeddedSlice>> Create(
        codec::CellKeyCodec codec, EmbeddedSliceOptions options = {});
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<EmbeddedSlice>> Reopen(
        codec::CellKeyCodec codec, EmbeddedSliceRecovery recovery);

    [[nodiscard]] absl::StatusOr<Timestamp> mutate(const std::vector<sstv2::types::Value>& row_key,
                                                   std::span<const CellMutation> mutations);

    // Logical reads return only non-deleted Cells. A row that never existed and
    // a row whose Cells are all hidden by tombstones both return nullopt.
    [[nodiscard]] absl::StatusOr<std::optional<VisibleRow>> get(
        const std::vector<sstv2::types::Value>& row_key, Timestamp read_ts) const;
    [[nodiscard]] absl::StatusOr<std::vector<VisibleRow>> scan(
        const std::optional<std::vector<sstv2::types::Value>>& start_row_key,
        const std::optional<std::vector<sstv2::types::Value>>& end_row_key,
        Timestamp read_ts) const;

    [[nodiscard]] absl::Status freeze();
    [[nodiscard]] absl::Status flush(FlushSstOptions options);

    [[nodiscard]] uint64_t last_timestamp_counter() const;
    [[nodiscard]] const PersistedManifest& persisted_manifest() const noexcept {
        return store_->persisted_manifest();
    }
    [[nodiscard]] SliceStore& storage_for_testing() noexcept { return *store_; }

private:
    EmbeddedSlice(codec::CellKeyCodec codec,
                  EmbeddedSliceOptions options,
                  std::unique_ptr<SliceStore> store,
                  uint64_t last_timestamp_counter)
        : codec_(std::move(codec)),
          options_(std::move(options)),
          store_(std::move(store)),
          last_timestamp_counter_(last_timestamp_counter) {}

    [[nodiscard]] absl::StatusOr<std::vector<VisibleRow>> scan_encoded(
        std::optional<std::string> start,
        std::optional<std::string> end,
        Timestamp read_ts,
        std::optional<std::string_view> exact_row = std::nullopt) const;

    // Serializes timestamp allocation, writes, freeze and flush into one writer
    // state machine. Reads remain lock-free through SliceStore read views.
    mutable std::mutex writer_mutex_;
    codec::CellKeyCodec codec_;
    EmbeddedSliceOptions options_;
    std::unique_ptr<SliceStore> store_;
    uint64_t last_timestamp_counter_ = 0;
};

} // namespace pl::minitable
