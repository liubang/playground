// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/slice_snapshot.h"

#include <limits>
#include <new>
#include <set>
#include <utility>

#include "absl/status/status.h"
#include "cpp/pl/sstv2/codec/fixed.h"

namespace pl::minitable {
namespace {

namespace snapshot_proto = ::pl::minitable::proto::v2::internal;

void SetFileIdentity(const sstv2::io::FileIdentity& source, snapshot_proto::FileIdentity* target) {
    target->set_inode_id(source.file_id);
    target->set_content_generation(source.content_generation);
    target->set_length(source.length);
    target->set_checksum(source.checksum);
}

sstv2::io::FileIdentity GetFileIdentity(const snapshot_proto::FileIdentity& source) {
    return {.file_id = source.inode_id(),
            .content_generation = source.content_generation(),
            .length = source.length(),
            .checksum = source.checksum(),
            .checksum_valid = true};
}

uint64_t SnapshotChecksum(const snapshot_proto::SliceSnapshot& snapshot) {
    snapshot_proto::SliceSnapshot copy = snapshot;
    copy.set_snapshot_checksum(0);
    std::string encoded;
    copy.SerializeToString(&encoded);
    return sstv2::codec::crc32c_u64(encoded);
}

absl::Status ValidateMetadata(const SliceSnapshotMetadata& metadata) {
    if (metadata.table_id == 0 || metadata.slice_id == 0 || metadata.schema_version == 0 ||
        metadata.route_epoch == 0 || metadata.replica_set_epoch == 0 ||
        metadata.last_included_index == 0 || metadata.last_included_term == 0 ||
        metadata.dedupe_retention_floor > metadata.last_included_index) {
        return absl::InvalidArgumentError("SliceSnapshot metadata is incomplete");
    }
    return absl::OkStatus();
}

} // namespace

absl::StatusOr<std::string> EncodeSliceSnapshot(const SliceApplyMachine& machine,
                                                const SliceSnapshotMetadata& metadata) {
    auto status = ValidateMetadata(metadata);
    if (!status.ok()) {
        return status;
    }
    const auto& store = machine.store();
    if (metadata.last_included_index != store.visible_applied_index() ||
        !store.can_snapshot_at(metadata.last_included_index)) {
        return absl::FailedPreconditionError(
            "Snapshot index must equal visible apply index and be flushed by every locality group");
    }
    const auto& manifest = store.read_view().manifest();
    snapshot_proto::SliceSnapshot snapshot;
    snapshot.set_format_version(kSliceSnapshotFormatVersion);
    snapshot.set_last_included_index(metadata.last_included_index);
    snapshot.set_last_included_term(metadata.last_included_term);
    snapshot.set_table_id(metadata.table_id);
    snapshot.set_slice_id(metadata.slice_id);
    snapshot.set_schema_version(metadata.schema_version);
    snapshot.set_route_epoch(metadata.route_epoch);
    snapshot.set_replica_set_epoch(metadata.replica_set_epoch);
    snapshot.mutable_timestamp_high_watermark()->set_domain_epoch(manifest.timestamp_domain_epoch);
    snapshot.mutable_timestamp_high_watermark()->set_counter(manifest.timestamp_high_watermark);
    snapshot.set_visible_applied_index(store.visible_applied_index());
    snapshot.set_timestamp_domain_epoch(manifest.timestamp_domain_epoch);
    snapshot.set_last_commit_physical_ms(manifest.last_commit_physical_ms);
    snapshot.set_dedupe_retention_floor(metadata.dedupe_retention_floor);
    snapshot.set_comparator_domain_fingerprint(manifest.comparator_domain.fingerprint);
    if (!store.persisted_manifest().path.empty()) {
        snapshot.set_manifest_path(store.persisted_manifest().path);
        SetFileIdentity(store.persisted_manifest().identity,
                        snapshot.mutable_manifest_object_identity());
        snapshot.set_manifest_generation(store.persisted_manifest().generation);
    }
    for (const auto& [group_id, group] : manifest.locality_groups) {
        auto* target = snapshot.add_locality_groups();
        target->set_locality_group_id(group_id);
        target->set_manifest_generation(manifest.generation);
        target->set_flushed_applied_index(group.flushed_applied_index);
        for (const auto& sst : group.ssts) {
            auto* object = target->add_ssts();
            object->set_sst_id(sst.sequence);
            object->set_key_file(sst.identity.key_path);
            object->set_value_file(sst.identity.value_path);
            object->set_key_file_size(sst.identity.key_file.length);
            object->set_value_file_size(sst.identity.value_file.length);
            object->set_key_checksum(sst.identity.key_file.checksum);
            object->set_value_checksum(sst.identity.value_file.checksum);
            object->set_row_count(sst.identity.row_count);
            SetFileIdentity(sst.identity.key_file, object->mutable_key_file_identity());
            SetFileIdentity(sst.identity.value_file, object->mutable_value_file_identity());
        }
    }
    for (const auto& record : machine.export_dedupe_records()) {
        if (record.applied_index < metadata.dedupe_retention_floor) {
            continue;
        }
        auto* target = snapshot.add_dedupe_records();
        target->mutable_identity()->set_client_id(record.identity.client_id);
        target->mutable_identity()->set_request_id(record.identity.request_id);
        target->mutable_identity()->set_payload_hash(record.identity.payload_hash);
        target->set_serialized_response(record.serialized_response);
        target->set_applied_index(record.applied_index);
        target->set_commit_physical_ms(record.commit_physical_ms);
    }
    snapshot.set_snapshot_checksum(SnapshotChecksum(snapshot));
    std::string encoded;
    if (!snapshot.SerializeToString(&encoded)) {
        return absl::InternalError("failed to serialize SliceSnapshot");
    }
    return encoded;
}

absl::StatusOr<LoadedSliceSnapshot> InstallSliceSnapshot(
    std::span<const std::byte> encoded,
    std::map<uint32_t, MemTableOptions> locality_groups,
    SliceStorePersistence persistence) {
    snapshot_proto::SliceSnapshot snapshot;
    if (encoded.empty() ||
        !snapshot.ParseFromArray(encoded.data(), static_cast<int>(encoded.size()))) {
        return absl::DataLossError("invalid SliceSnapshot protobuf");
    }
    if (snapshot.format_version() != kSliceSnapshotFormatVersion ||
        snapshot.snapshot_checksum() == 0 ||
        snapshot.snapshot_checksum() != SnapshotChecksum(snapshot)) {
        return absl::DataLossError("SliceSnapshot format or checksum mismatch");
    }
    SliceSnapshotMetadata metadata{.table_id = snapshot.table_id(),
                                   .slice_id = snapshot.slice_id(),
                                   .schema_version = snapshot.schema_version(),
                                   .route_epoch = snapshot.route_epoch(),
                                   .replica_set_epoch = snapshot.replica_set_epoch(),
                                   .last_included_index = snapshot.last_included_index(),
                                   .last_included_term = snapshot.last_included_term(),
                                   .dedupe_retention_floor = snapshot.dedupe_retention_floor()};
    auto status = ValidateMetadata(metadata);
    if (!status.ok() || snapshot.visible_applied_index() != metadata.last_included_index ||
        snapshot.timestamp_domain_epoch() != persistence.timestamp_domain_epoch ||
        snapshot.timestamp_high_watermark().domain_epoch() != persistence.timestamp_domain_epoch ||
        snapshot.comparator_domain_fingerprint() != persistence.comparator_domain.fingerprint ||
        snapshot.locality_groups_size() != static_cast<int>(locality_groups.size()) ||
        !snapshot.has_manifest_object_identity() || snapshot.manifest_path().empty()) {
        return absl::FailedPreconditionError("SliceSnapshot does not match the target Slice domain");
    }
    uint64_t minimum_flushed = std::numeric_limits<uint64_t>::max();
    std::set<uint32_t> seen_groups;
    for (const auto& group : snapshot.locality_groups()) {
        if (!locality_groups.contains(group.locality_group_id()) ||
            !seen_groups.insert(group.locality_group_id()).second) {
            return absl::FailedPreconditionError("SliceSnapshot locality group set mismatch");
        }
        minimum_flushed = std::min(minimum_flushed, group.flushed_applied_index());
        for (const auto& sst : group.ssts()) {
            auto key = persistence.filesystem->open(sst.key_file(), GetFileIdentity(sst.key_file_identity()));
            if (!key.ok()) {
                return key.status();
            }
            auto key_close = persistence.filesystem->close(*key);
            if (!key_close.ok()) {
                return key_close.status();
            }
            auto value = persistence.filesystem->open(
                sst.value_file(), GetFileIdentity(sst.value_file_identity()));
            if (!value.ok()) {
                return value.status();
            }
            auto value_close = persistence.filesystem->close(*value);
            if (!value_close.ok()) {
                return value_close.status();
            }
        }
    }
    if (metadata.last_included_index > minimum_flushed) {
        return absl::FailedPreconditionError("SliceSnapshot advances beyond a locality group flush fence");
    }
    PersistedManifest persisted{.path = snapshot.manifest_path(),
                                .identity = GetFileIdentity(snapshot.manifest_object_identity()),
                                .generation = snapshot.manifest_generation()};
    auto store = SliceStore::Reopen({.locality_groups = std::move(locality_groups),
                                     .persistence = std::move(persistence),
                                     .manifest = std::move(persisted)});
    if (!store.ok()) {
        return store.status();
    }
    if ((*store)->visible_applied_index() != metadata.last_included_index ||
        (*store)->timestamp_high_watermark() != snapshot.timestamp_high_watermark().counter() ||
        (*store)->last_commit_physical_ms() != snapshot.last_commit_physical_ms()) {
        return absl::DataLossError("SliceSnapshot and authoritative Manifest disagree");
    }
    SliceApplyRecovery recovery;
    try {
        recovery.dedupe_records.reserve(static_cast<size_t>(snapshot.dedupe_records_size()));
        for (const auto& source : snapshot.dedupe_records()) {
            if (source.applied_index() < metadata.dedupe_retention_floor ||
                source.applied_index() > metadata.last_included_index) {
                return absl::DataLossError("SliceSnapshot contains an out-of-range dedupe record");
            }
            recovery.dedupe_records.push_back(
                {.identity = {.client_id = source.identity().client_id(),
                              .request_id = source.identity().request_id(),
                              .payload_hash = source.identity().payload_hash()},
                 .serialized_response = source.serialized_response(),
                 .applied_index = source.applied_index(),
                 .commit_physical_ms = source.commit_physical_ms()});
        }
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SliceSnapshot recovery allocation failed");
    }
    auto machine = SliceApplyMachine::Create(std::move(*store), std::move(recovery));
    if (!machine.ok()) {
        return machine.status();
    }
    return LoadedSliceSnapshot{.machine = std::move(*machine), .metadata = metadata};
}

} // namespace pl::minitable
