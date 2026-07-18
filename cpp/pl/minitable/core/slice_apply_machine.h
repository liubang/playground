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
// Created: 2026/07/18 09:56

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/key.h"
#include "cpp/pl/minitable/core/slice_store.h"

namespace pl::minitable {

struct MutationIdentity {
    std::string client_id;
    std::string request_id;
    uint64_t payload_hash = 0;

    bool operator==(const MutationIdentity&) const = default;
};

struct CommittedMemTableMutation {
    std::string encoded_key;
    std::string encoded_value;

    bool operator==(const CommittedMemTableMutation&) const = default;
};

struct CommittedSliceMutation {
    uint64_t apply_index = 0;
    MutationIdentity identity;
    Timestamp commit_ts;
    uint64_t commit_physical_ms = 0;
    std::vector<std::vector<CommittedMemTableMutation>> locality_group_mutations;
    std::vector<uint32_t> locality_group_ids;
    std::string serialized_response;
};

struct DedupeRecord {
    MutationIdentity identity;
    std::string serialized_response;
    uint64_t applied_index = 0;
    uint64_t commit_physical_ms = 0;

    bool operator==(const DedupeRecord&) const = default;
};

struct SliceApplyRecovery {
    std::vector<DedupeRecord> dedupe_records;
};

struct ApplyResult {
    bool duplicate = false;
    std::string serialized_response;
};

// Deterministic state-machine boundary used by a future braft on_apply callback.
// It consumes only values embedded in a committed entry and never reads clocks or allocates timestamps.
class SliceApplyMachine final {
public:
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<SliceApplyMachine>> Create(
        std::unique_ptr<SliceStore> store, SliceApplyRecovery recovery = {});

    [[nodiscard]] absl::StatusOr<ApplyResult> apply(const CommittedSliceMutation& mutation);
    [[nodiscard]] absl::StatusOr<ApplyResult> apply_serialized(
        std::span<const std::byte> encoded_entry,
        uint64_t apply_index,
        const codec::CellKeyCodec& codec);
    [[nodiscard]] std::vector<DedupeRecord> export_dedupe_records() const;

    [[nodiscard]] SliceStore& store() noexcept { return *store_; }
    [[nodiscard]] const SliceStore& store() const noexcept { return *store_; }

private:
    struct DedupeKey {
        std::string client_id;
        std::string request_id;
        auto operator<=>(const DedupeKey&) const = default;
    };

    SliceApplyMachine(std::unique_ptr<SliceStore> store,
                      std::map<DedupeKey, DedupeRecord> dedupe)
        : store_(std::move(store)), dedupe_(std::move(dedupe)) {}

    mutable std::mutex mutex_;
    std::unique_ptr<SliceStore> store_;
    std::map<DedupeKey, DedupeRecord> dedupe_;
};

[[nodiscard]] absl::StatusOr<std::string> EncodeSliceMutationV2(
    const CommittedSliceMutation& mutation,
    std::string_view encoded_logical_row_key,
    const codec::CellKeyCodec& codec);
[[nodiscard]] absl::StatusOr<CommittedSliceMutation> DecodeSliceMutationV2(
    std::span<const std::byte> encoded_entry,
    uint64_t apply_index,
    const codec::CellKeyCodec& codec);

} // namespace pl::minitable
