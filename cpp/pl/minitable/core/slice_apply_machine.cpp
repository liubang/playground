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

#include "cpp/pl/minitable/core/slice_apply_machine.h"

#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <utility>

#include "absl/status/status.h"
#include "cpp/pl/minitable/proto/v2/internal.pb.h"
#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {
namespace {

absl::Status ValidateIdentity(const MutationIdentity& identity) {
    if (identity.client_id.empty() || identity.request_id.empty() || identity.payload_hash == 0) {
        return absl::InvalidArgumentError("committed mutation requires a complete identity");
    }
    return absl::OkStatus();
}

namespace internal_proto = ::pl::minitable::proto::v2::internal;
namespace public_proto = ::pl::minitable::proto::v2;
using sstv2::types::OpType;

absl::StatusOr<CellRef> DecodeCellRef(const public_proto::CellRef& source) {
    if (source.column_family_id() == 0) {
        return absl::InvalidArgumentError("proto CellRef requires a column family ID");
    }
    CellRef ref{.column_family_id = source.column_family_id()};
    switch (source.qualifier_case()) {
        case public_proto::CellRef::kStaticQualifier:
            if (source.static_qualifier().column_id() == 0) {
                return absl::InvalidArgumentError("proto static qualifier requires a column ID");
            }
            ref.qualifier = StaticQualifier{.column_id = source.static_qualifier().column_id()};
            break;
        case public_proto::CellRef::kDynamicQualifier:
            ref.qualifier = DynamicQualifier{.value = source.dynamic_qualifier().qualifier()};
            break;
        default:
            return absl::InvalidArgumentError("proto CellRef qualifier is missing");
    }
    return ref;
}

void EncodeCellRef(const CellRef& source, public_proto::CellRef* target) {
    target->set_column_family_id(source.column_family_id);
    if (const auto* qualifier = std::get_if<StaticQualifier>(&source.qualifier)) {
        target->mutable_static_qualifier()->set_column_id(qualifier->column_id);
    } else {
        target->mutable_dynamic_qualifier()->set_qualifier(
            std::get<DynamicQualifier>(source.qualifier).value);
    }
}

} // namespace

absl::StatusOr<std::unique_ptr<SliceApplyMachine>> SliceApplyMachine::Create(
    std::unique_ptr<SliceStore> store, SliceApplyRecovery recovery) {
    if (store == nullptr) {
        return absl::InvalidArgumentError("SliceApplyMachine requires a SliceStore");
    }
    try {
        std::map<DedupeKey, DedupeRecord> dedupe;
        for (auto& record : recovery.dedupe_records) {
            auto status = ValidateIdentity(record.identity);
            if (!status.ok() || record.serialized_response.empty() || record.applied_index == 0 ||
                record.applied_index > store->visible_applied_index() ||
                record.commit_physical_ms > store->last_commit_physical_ms()) {
                return absl::FailedPreconditionError("invalid recovered dedupe record");
            }
            DedupeKey key{.client_id = record.identity.client_id,
                          .request_id = record.identity.request_id};
            if (!dedupe.emplace(std::move(key), std::move(record)).second) {
                return absl::FailedPreconditionError("duplicate recovered dedupe identity");
            }
        }
        return std::unique_ptr<SliceApplyMachine>(
            new SliceApplyMachine(std::move(store), std::move(dedupe)));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SliceApplyMachine recovery allocation failed");
    }
}

absl::StatusOr<ApplyResult> SliceApplyMachine::apply_serialized(
    std::span<const std::byte> encoded_entry,
    uint64_t apply_index,
    const codec::CellKeyCodec& codec) {
    auto mutation = DecodeSliceMutationV2(encoded_entry, apply_index, codec);
    return mutation.ok() ? apply(*mutation) : absl::StatusOr<ApplyResult>(mutation.status());
}

absl::StatusOr<ApplyResult> SliceApplyMachine::apply(const CommittedSliceMutation& mutation) {
    std::lock_guard lock(mutex_);
    auto status = ValidateIdentity(mutation.identity);
    if (!status.ok()) {
        return status;
    }
    if (mutation.apply_index == 0 || mutation.commit_ts.domain_epoch == 0 ||
        mutation.commit_ts.counter == 0 || mutation.commit_physical_ms == 0 ||
        mutation.serialized_response.empty() ||
        mutation.locality_group_ids.size() != mutation.locality_group_mutations.size()) {
        return absl::InvalidArgumentError("invalid committed Slice mutation");
    }
    if (mutation.commit_ts.domain_epoch != store_->read_view().manifest().timestamp_domain_epoch) {
        return absl::FailedPreconditionError("committed timestamp belongs to another Slice domain");
    }
    if (mutation.apply_index <= store_->visible_applied_index()) {
        return absl::InvalidArgumentError("committed apply index must advance");
    }

    const DedupeKey key{.client_id = mutation.identity.client_id,
                        .request_id = mutation.identity.request_id};
    const auto existing = dedupe_.find(key);
    if (existing != dedupe_.end()) {
        status = store_->apply_committed(
            {},
            mutation.apply_index,
            std::max(store_->timestamp_high_watermark(), mutation.commit_ts.counter),
            std::max(store_->last_commit_physical_ms(), mutation.commit_physical_ms));
        if (!status.ok()) {
            return status;
        }
        if (existing->second.identity.payload_hash != mutation.identity.payload_hash) {
            return absl::AlreadyExistsError("mutation identity was reused with another payload");
        }
        return ApplyResult{.duplicate = true,
                           .serialized_response = existing->second.serialized_response};
    }

    try {
        std::set<uint32_t> seen_groups;
        std::vector<std::vector<MemTableMutation>> mutation_views(
            mutation.locality_group_ids.size());
        std::vector<LocalityGroupPatch> patches;
        patches.reserve(mutation.locality_group_ids.size());
        for (size_t i = 0; i < mutation.locality_group_ids.size(); ++i) {
            const uint32_t group_id = mutation.locality_group_ids[i];
            if (group_id == 0 || mutation.locality_group_mutations[i].empty() ||
                !seen_groups.insert(group_id).second) {
                return absl::InvalidArgumentError("invalid committed locality group patch");
            }
            auto& views = mutation_views[i];
            views.reserve(mutation.locality_group_mutations[i].size());
            for (const auto& entry : mutation.locality_group_mutations[i]) {
                views.push_back(
                    {.encoded_key = entry.encoded_key, .encoded_value = entry.encoded_value});
            }
            patches.push_back({.locality_group_id = group_id, .mutations = views});
        }
        if (patches.empty()) {
            return absl::InvalidArgumentError("new committed mutation has no writes");
        }

        DedupeRecord record{.identity = mutation.identity,
                            .serialized_response = mutation.serialized_response,
                            .applied_index = mutation.apply_index,
                            .commit_physical_ms = mutation.commit_physical_ms};
        auto [record_it, inserted] = dedupe_.emplace(key, std::move(record));
        if (!inserted) {
            return absl::InternalError("dedupe insertion raced under apply serialization");
        }
        status = store_->apply_committed(
            patches, mutation.apply_index, mutation.commit_ts.counter, mutation.commit_physical_ms);
        if (!status.ok()) {
            dedupe_.erase(record_it);
            return status;
        }
        return ApplyResult{.serialized_response = mutation.serialized_response};
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("committed mutation allocation failed");
    }
}

absl::StatusOr<std::string> EncodeSliceMutationV2(const CommittedSliceMutation& mutation,
                                                  std::string_view encoded_logical_row_key,
                                                  const codec::CellKeyCodec& codec) {
    auto status = ValidateIdentity(mutation.identity);
    if (!status.ok()) {
        return status;
    }
    if (mutation.commit_ts.domain_epoch == 0 || mutation.commit_ts.counter == 0 ||
        mutation.commit_physical_ms == 0 || mutation.serialized_response.empty() ||
        mutation.locality_group_ids.size() != mutation.locality_group_mutations.size() ||
        mutation.locality_group_ids.empty()) {
        return absl::InvalidArgumentError("cannot encode incomplete committed Slice mutation");
    }
    std::set<uint32_t> group_ids;
    for (size_t index = 0; index < mutation.locality_group_ids.size(); ++index) {
        if (mutation.locality_group_ids[index] == 0 ||
            mutation.locality_group_mutations[index].empty() ||
            !group_ids.insert(mutation.locality_group_ids[index]).second) {
            return absl::InvalidArgumentError("cannot encode invalid locality group patches");
        }
    }
    auto decoded_row = codec.DecodeLogicalRowKey(encoded_logical_row_key);
    if (!decoded_row.ok()) {
        return decoded_row.status();
    }
    internal_proto::SliceMutation proto;
    proto.set_format_version(1);
    proto.mutable_identity()->set_client_id(mutation.identity.client_id);
    proto.mutable_identity()->set_request_id(mutation.identity.request_id);
    proto.mutable_identity()->set_payload_hash(mutation.identity.payload_hash);
    proto.set_serialized_response(mutation.serialized_response);
    auto* transaction = proto.mutable_row_transaction();
    transaction->set_encoded_row_key(encoded_logical_row_key);
    transaction->mutable_commit_ts()->set_domain_epoch(mutation.commit_ts.domain_epoch);
    transaction->mutable_commit_ts()->set_counter(mutation.commit_ts.counter);
    transaction->set_commit_physical_ms(mutation.commit_physical_ms);
    for (size_t group_index = 0; group_index < mutation.locality_group_ids.size(); ++group_index) {
        auto* group = transaction->add_locality_groups();
        group->set_locality_group_id(mutation.locality_group_ids[group_index]);
        for (const auto& mutation_entry : mutation.locality_group_mutations[group_index]) {
            auto decoded_key = codec.DecodeVersionedStorageKey(mutation_entry.encoded_key);
            if (!decoded_key.ok()) {
                return decoded_key.status();
            }
            if (decoded_key->storage_key.row_key != *decoded_row ||
                decoded_key->commit_ts != mutation.commit_ts) {
                return absl::InvalidArgumentError(
                    "mutation key does not match canonical row transaction");
            }
            if (decoded_key->op_type == OpType::kMerge) {
                return absl::InvalidArgumentError(
                    "SliceMutation v2 does not support merge operations");
            }
            auto* canonical = group->add_mutations();
            canonical->set_mutation_seq(decoded_key->mutation_seq);
            if (std::holds_alternative<RowTombstone>(decoded_key->storage_key.target)) {
                canonical->set_delete_row(true);
            } else if (const auto* tombstone =
                           std::get_if<ColumnFamilyTombstone>(&decoded_key->storage_key.target)) {
                canonical->set_delete_column_family_id(tombstone->column_family_id);
            } else {
                auto* cell = canonical->mutable_cell();
                EncodeCellRef(std::get<CellRef>(decoded_key->storage_key.target),
                              cell->mutable_cell());
                cell->set_type(decoded_key->op_type == OpType::kPut ? public_proto::SET
                                                                    : public_proto::DELETE_COLUMN);
                if (decoded_key->op_type == OpType::kPut) {
                    cell->mutable_value()->set_bytes_value(mutation_entry.encoded_value);
                }
            }
        }
    }
    std::string encoded;
    if (!proto.SerializeToString(&encoded)) {
        return absl::InternalError("failed to serialize SliceMutation v2");
    }
    return encoded;
}

absl::StatusOr<CommittedSliceMutation> DecodeSliceMutationV2(
    std::span<const std::byte> encoded_entry,
    uint64_t apply_index,
    const codec::CellKeyCodec& codec) {
    if (apply_index == 0 || encoded_entry.empty()) {
        return absl::InvalidArgumentError("committed log entry is empty or has zero index");
    }
    internal_proto::SliceMutation proto;
    if (!proto.ParseFromArray(encoded_entry.data(), static_cast<int>(encoded_entry.size()))) {
        return absl::DataLossError("invalid SliceMutation v2 protobuf");
    }
    if (proto.format_version() != 1 || !proto.has_identity() ||
        proto.operation_case() != internal_proto::SliceMutation::kRowTransaction ||
        proto.serialized_response().empty()) {
        return absl::FailedPreconditionError("unsupported or incomplete SliceMutation v2");
    }
    const auto& transaction = proto.row_transaction();
    auto row_key = codec.DecodeLogicalRowKey(transaction.encoded_row_key());
    if (!row_key.ok()) {
        return row_key.status();
    }
    CommittedSliceMutation result{
        .apply_index = apply_index,
        .identity = {.client_id = proto.identity().client_id(),
                     .request_id = proto.identity().request_id(),
                     .payload_hash = proto.identity().payload_hash()},
        .commit_ts = {.domain_epoch = transaction.commit_ts().domain_epoch(),
                      .counter = transaction.commit_ts().counter()},
        .commit_physical_ms = transaction.commit_physical_ms(),
        .serialized_response = proto.serialized_response(),
    };
    auto status = ValidateIdentity(result.identity);
    if (!status.ok()) {
        return status;
    }
    if (!transaction.has_commit_ts() || result.commit_ts.domain_epoch == 0 ||
        result.commit_ts.counter == 0 || result.commit_physical_ms == 0) {
        return absl::InvalidArgumentError("row transaction requires a complete commit timestamp");
    }
    std::set<uint32_t> group_ids;
    try {
        for (const auto& group : transaction.locality_groups()) {
            if (group.locality_group_id() == 0 || group.mutations().empty() ||
                !group_ids.insert(group.locality_group_id()).second) {
                return absl::InvalidArgumentError("invalid canonical locality group patch");
            }
            std::set<uint32_t> sequences;
            std::vector<CommittedMemTableMutation> mutations;
            mutations.reserve(static_cast<size_t>(group.mutations_size()));
            for (const auto& canonical : group.mutations()) {
                if (!sequences.insert(canonical.mutation_seq()).second) {
                    return absl::InvalidArgumentError(
                        "duplicate mutation sequence in row transaction");
                }
                VersionedStorageKey key{
                    .storage_key = {.partition = GlobalOrderPrefix{}, .row_key = *row_key},
                    .commit_ts = result.commit_ts,
                    .mutation_seq = canonical.mutation_seq(),
                    .op_type = OpType::kDelete};
                std::string value;
                switch (canonical.operation_case()) {
                    case internal_proto::CanonicalMutation::kDeleteRow:
                        if (!canonical.delete_row()) {
                            return absl::InvalidArgumentError("delete_row must be true");
                        }
                        key.storage_key.target = RowTombstone{};
                        break;
                    case internal_proto::CanonicalMutation::kDeleteColumnFamilyId:
                        if (canonical.delete_column_family_id() == 0) {
                            return absl::InvalidArgumentError("column family tombstone ID is zero");
                        }
                        key.storage_key.target = ColumnFamilyTombstone{
                            .column_family_id = canonical.delete_column_family_id()};
                        break;
                    case internal_proto::CanonicalMutation::kCell: {
                        auto cell_ref = DecodeCellRef(canonical.cell().cell());
                        if (!cell_ref.ok()) {
                            return cell_ref.status();
                        }
                        key.storage_key.target = std::move(*cell_ref);
                        if (canonical.cell().type() == public_proto::SET) {
                            if (!canonical.cell().has_value()) {
                                return absl::InvalidArgumentError("PUT is missing its value");
                            }
                            if (canonical.cell().value().kind_case() !=
                                public_proto::Value::kBytesValue) {
                                return absl::InvalidArgumentError(
                                    "canonical storage value must use bytes_value");
                            }
                            key.op_type = OpType::kPut;
                            value = canonical.cell().value().bytes_value();
                        } else if (canonical.cell().type() != public_proto::DELETE_COLUMN ||
                                   canonical.cell().has_value()) {
                            return absl::InvalidArgumentError("invalid canonical cell mutation");
                        }
                        break;
                    }
                    default:
                        return absl::InvalidArgumentError(
                            "canonical mutation operation is missing");
                }
                auto encoded_key = codec.EncodeVersionedStorageKey(key);
                if (!encoded_key.ok()) {
                    return encoded_key.status();
                }
                mutations.push_back(
                    {.encoded_key = std::move(*encoded_key), .encoded_value = std::move(value)});
            }
            result.locality_group_ids.push_back(group.locality_group_id());
            result.locality_group_mutations.push_back(std::move(mutations));
        }
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SliceMutation v2 decode allocation failed");
    }
    if (result.locality_group_ids.empty()) {
        return absl::InvalidArgumentError("row transaction has no locality group writes");
    }
    return result;
}

DedupeLookupResult SliceApplyMachine::lookup_dedupe(const MutationIdentity& identity) const {
    std::lock_guard lock(mutex_);
    const auto it =
        dedupe_.find(DedupeKey{.client_id = identity.client_id, .request_id = identity.request_id});
    if (it == dedupe_.end()) {
        return {};
    }
    if (it->second.identity.payload_hash != identity.payload_hash) {
        return {.kind = DedupeLookupKind::kConflict};
    }
    return {.kind = DedupeLookupKind::kDuplicate,
            .serialized_response = it->second.serialized_response};
}

std::vector<DedupeRecord> SliceApplyMachine::export_dedupe_records() const {
    std::lock_guard lock(mutex_);
    std::vector<DedupeRecord> records;
    records.reserve(dedupe_.size());
    for (const auto& [key, record] : dedupe_) {
        static_cast<void>(key);
        records.push_back(record);
    }
    return records;
}

} // namespace pl::minitable
