// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include "cpp/pl/minitable/core/manifest.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "cpp/pl/sstv2/codec/fixed.h"

namespace pl::minitable {
namespace {

constexpr std::string_view kMagic = "MTMF";
constexpr uint64_t kMaxManifestBytes = 64ULL * 1024 * 1024;
constexpr uint32_t kMaxLocalityGroups = 65536;
constexpr uint32_t kMaxSstsPerLocalityGroup = 1'000'000;
constexpr uint32_t kMaxPathLength = 16 * 1024;

void AppendIdentity(std::string* out, const sstv2::io::FileIdentity& identity) {
    sstv2::codec::append_fixed64(out, identity.file_id);
    sstv2::codec::append_fixed64(out, identity.content_generation);
    sstv2::codec::append_fixed64(out, identity.length);
    sstv2::codec::append_fixed64(out, identity.checksum);
    sstv2::codec::append_fixed8(out, identity.checksum_valid ? 1 : 0);
}

class Decoder final {
public:
    explicit Decoder(std::string_view input) : input_(input) {}

    absl::StatusOr<uint8_t> u8() {
        if (remaining() < 1) {
            return absl::DataLossError("truncated manifest uint8");
        }
        return static_cast<uint8_t>(input_[offset_++]);
    }
    absl::StatusOr<uint32_t> u32() {
        if (remaining() < sizeof(uint32_t)) {
            return absl::DataLossError("truncated manifest uint32");
        }
        const auto value = sstv2::codec::read_fixed32(input_, offset_);
        offset_ += sizeof(uint32_t);
        return value;
    }
    absl::StatusOr<uint64_t> u64() {
        if (remaining() < sizeof(uint64_t)) {
            return absl::DataLossError("truncated manifest uint64");
        }
        const auto value = sstv2::codec::read_fixed64(input_, offset_);
        offset_ += sizeof(uint64_t);
        return value;
    }
    absl::StatusOr<std::string> string() {
        auto length = u32();
        if (!length.ok()) {
            return length.status();
        }
        if (*length > kMaxPathLength || remaining() < *length) {
            return absl::DataLossError("invalid manifest string length");
        }
        std::string value(input_.substr(offset_, *length));
        offset_ += *length;
        return value;
    }
    absl::StatusOr<sstv2::io::FileIdentity> identity() {
        auto file_id = u64();
        auto generation = u64();
        auto length = u64();
        auto checksum = u64();
        auto valid = u8();
        if (!file_id.ok() || !generation.ok() || !length.ok() || !checksum.ok() || !valid.ok()) {
            return absl::DataLossError("truncated manifest file identity");
        }
        if (*valid > 1 || *file_id == 0 || *valid == 0) {
            return absl::DataLossError("invalid manifest file identity");
        }
        return sstv2::io::FileIdentity{.file_id = *file_id,
                                       .content_generation = *generation,
                                       .length = *length,
                                       .checksum = *checksum,
                                       .checksum_valid = true};
    }
    [[nodiscard]] size_t remaining() const { return input_.size() - offset_; }

private:
    std::string_view input_;
    size_t offset_ = 0;
};

void AppendString(std::string* out, std::string_view value) {
    sstv2::codec::append_fixed32(out, static_cast<uint32_t>(value.size()));
    out->append(value);
}

uint64_t ManifestEditFingerprint(const ManifestEdit& edit) {
    std::string encoded;
    sstv2::codec::append_fixed64(&encoded, edit.edit_id);
    sstv2::codec::append_fixed32(&encoded, edit.locality_group_id);
    sstv2::codec::append_fixed64(&encoded, edit.parent_generation);
    sstv2::codec::append_fixed64(&encoded, edit.new_generation);
    sstv2::codec::append_fixed64(&encoded, edit.immutable_id);
    sstv2::codec::append_fixed64(&encoded, edit.immutable_fence_index);
    sstv2::codec::append_fixed64(&encoded, edit.timestamp_high_watermark);
    sstv2::codec::append_fixed64(&encoded, edit.last_commit_physical_ms);
    for (const auto& output : edit.outputs) {
        AppendString(&encoded, output.key_path);
        AppendString(&encoded, output.value_path);
        AppendIdentity(&encoded, output.key_file);
        AppendIdentity(&encoded, output.value_file);
        sstv2::codec::append_fixed64(&encoded, output.row_count);
        sstv2::codec::append_fixed64(&encoded, output.sst_format_version);
        sstv2::codec::append_fixed64(&encoded, output.comparator_domain.fingerprint);
        sstv2::codec::append_fixed64(&encoded, output.checksum_algorithm);
    }
    return sstv2::codec::crc32c_u64(encoded);
}

absl::Status ValidateManifest(const ManifestRef& manifest) {
    if (manifest.format_version != kManifestFormatVersion || manifest.generation == 0 ||
        manifest.next_sst_sequence == 0 || manifest.locality_groups.empty() ||
        manifest.locality_groups.size() > kMaxLocalityGroups ||
        manifest.installed_edits.size() > kMaxInstalledEditLedgerEntries ||
        !IsValidComparatorDomain(manifest.comparator_domain) ||
        manifest.timestamp_domain_epoch == 0) {
        return absl::InvalidArgumentError("invalid manifest header");
    }
    uint64_t previous_edit_id = 0;
    for (const auto& edit : manifest.installed_edits) {
        if (edit.edit_id == 0 || edit.edit_id <= previous_edit_id ||
            edit.result_generation > manifest.generation || edit.semantic_fingerprint == 0) {
            return absl::InvalidArgumentError("invalid manifest install ledger");
        }
        previous_edit_id = edit.edit_id;
    }
    for (const auto& [locality_group_id, group] : manifest.locality_groups) {
        if (locality_group_id == 0 || group.ssts.size() > kMaxSstsPerLocalityGroup) {
            return absl::InvalidArgumentError("invalid manifest locality group");
        }
        uint64_t previous_sequence = std::numeric_limits<uint64_t>::max();
        for (const auto& sst : group.ssts) {
            const auto& identity = sst.identity;
            if (sst.sequence == 0 || sst.sequence >= manifest.next_sst_sequence ||
                sst.sequence >= previous_sequence || identity.key_path.empty() ||
                identity.value_path.empty() || identity.key_path == identity.value_path ||
                identity.key_path.size() > kMaxPathLength ||
                identity.value_path.size() > kMaxPathLength || identity.key_file.file_id == 0 ||
                identity.value_file.file_id == 0 || !identity.key_file.checksum_valid ||
                !identity.value_file.checksum_valid ||
                identity.sst_format_version != kMinitableSstFormatVersion ||
                identity.comparator_domain != manifest.comparator_domain ||
                identity.checksum_algorithm != kCrc32cChecksumAlgorithm) {
                return absl::InvalidArgumentError("invalid manifest SST entry");
            }
            previous_sequence = sst.sequence;
        }
    }
    return absl::OkStatus();
}

} // namespace

absl::StatusOr<std::shared_ptr<const ManifestRef>> ApplyManifestEdit(const ManifestRef& current,
                                                                     const ManifestEdit& edit) {
    auto status = ValidateManifest(current);
    if (!status.ok()) {
        return status;
    }
    if (edit.edit_id == 0 || edit.locality_group_id == 0 || edit.immutable_id == 0 ||
        edit.outputs.size() != 1 || edit.timestamp_high_watermark < current.timestamp_high_watermark ||
        edit.last_commit_physical_ms < current.last_commit_physical_ms) {
        return absl::InvalidArgumentError("invalid flush manifest edit");
    }
    const auto installed = std::lower_bound(
        current.installed_edits.begin(),
        current.installed_edits.end(),
        edit.edit_id,
        [](const InstalledManifestEdit& lhs, uint64_t edit_id) { return lhs.edit_id < edit_id; });
    const uint64_t semantic_fingerprint = ManifestEditFingerprint(edit);
    if (installed != current.installed_edits.end() && installed->edit_id == edit.edit_id) {
        return installed->result_generation == edit.new_generation &&
                       installed->semantic_fingerprint == semantic_fingerprint
                   ? std::make_shared<const ManifestRef>(current)
                   : absl::StatusOr<std::shared_ptr<const ManifestRef>>(
                         absl::AlreadyExistsError("manifest edit ID was reused"));
    }
    if (edit.parent_generation != current.generation ||
        edit.new_generation != current.generation + 1) {
        return absl::AbortedError("stale manifest generation CAS");
    }
    if (!current.locality_groups.contains(edit.locality_group_id) ||
        current.generation == std::numeric_limits<uint64_t>::max() ||
        current.next_sst_sequence == std::numeric_limits<uint64_t>::max()) {
        return absl::FailedPreconditionError("manifest edit cannot advance");
    }

    try {
        auto next = std::make_shared<ManifestRef>(current);
        auto& group = next->locality_groups.at(edit.locality_group_id);
        group.ssts.insert(
            group.ssts.begin(),
            ManifestSst{.sequence = next->next_sst_sequence, .identity = edit.outputs.front()});
        group.flushed_applied_index =
            std::max(group.flushed_applied_index, edit.immutable_fence_index);
        ++next->next_sst_sequence;
        next->generation = edit.new_generation;
        next->timestamp_high_watermark = edit.timestamp_high_watermark;
        next->last_commit_physical_ms = edit.last_commit_physical_ms;
        next->installed_edits.insert(
            std::lower_bound(next->installed_edits.begin(),
                             next->installed_edits.end(),
                             edit.edit_id,
                             [](const InstalledManifestEdit& lhs, uint64_t edit_id) {
                                 return lhs.edit_id < edit_id;
                             }),
            {.edit_id = edit.edit_id,
             .result_generation = edit.new_generation,
             .semantic_fingerprint = semantic_fingerprint});
        if (next->installed_edits.size() > kMaxInstalledEditLedgerEntries) {
            next->installed_edits.erase(next->installed_edits.begin());
        }
        status = ValidateManifest(*next);
        if (!status.ok()) {
            return status;
        }
        return std::shared_ptr<const ManifestRef>(std::move(next));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("manifest edit allocation failed");
    }
}

absl::StatusOr<std::string> EncodeManifest(const ManifestRef& manifest) {
    auto status = ValidateManifest(manifest);
    if (!status.ok()) {
        return status;
    }
    try {
        std::string payload;
        sstv2::codec::append_fixed32(&payload, manifest.format_version);
        sstv2::codec::append_fixed64(&payload, manifest.generation);
        sstv2::codec::append_fixed64(&payload, manifest.next_sst_sequence);
        sstv2::codec::append_fixed64(&payload, manifest.comparator_domain.key_format_version);
        sstv2::codec::append_fixed64(
            &payload, manifest.comparator_domain.row_key_schema_fingerprint);
        sstv2::codec::append_fixed64(&payload, manifest.comparator_domain.partition_mode);
        sstv2::codec::append_fixed64(&payload, manifest.comparator_domain.hash_algorithm_version);
        sstv2::codec::append_fixed64(&payload, manifest.comparator_domain.virtual_bucket_count);
        sstv2::codec::append_fixed64(&payload, manifest.comparator_domain.fingerprint);
        sstv2::codec::append_fixed64(&payload, manifest.timestamp_domain_epoch);
        sstv2::codec::append_fixed64(&payload, manifest.timestamp_high_watermark);
        sstv2::codec::append_fixed64(&payload, manifest.last_commit_physical_ms);
        sstv2::codec::append_fixed32(&payload,
                                     static_cast<uint32_t>(manifest.locality_groups.size()));
        for (const auto& [locality_group_id, group] : manifest.locality_groups) {
            sstv2::codec::append_fixed32(&payload, locality_group_id);
            sstv2::codec::append_fixed64(&payload, group.flushed_applied_index);
            sstv2::codec::append_fixed32(&payload, static_cast<uint32_t>(group.ssts.size()));
            for (const auto& sst : group.ssts) {
                sstv2::codec::append_fixed64(&payload, sst.sequence);
                AppendString(&payload, sst.identity.key_path);
                AppendString(&payload, sst.identity.value_path);
                AppendIdentity(&payload, sst.identity.key_file);
                AppendIdentity(&payload, sst.identity.value_file);
                sstv2::codec::append_fixed64(&payload, sst.identity.row_count);
                sstv2::codec::append_fixed64(&payload, sst.identity.sst_format_version);
                sstv2::codec::append_fixed64(&payload,
                                             sst.identity.comparator_domain.key_format_version);
                sstv2::codec::append_fixed64(
                    &payload, sst.identity.comparator_domain.row_key_schema_fingerprint);
                sstv2::codec::append_fixed64(&payload,
                                             sst.identity.comparator_domain.partition_mode);
                sstv2::codec::append_fixed64(&payload,
                                             sst.identity.comparator_domain.hash_algorithm_version);
                sstv2::codec::append_fixed64(&payload,
                                             sst.identity.comparator_domain.virtual_bucket_count);
                sstv2::codec::append_fixed64(&payload, sst.identity.comparator_domain.fingerprint);
                sstv2::codec::append_fixed64(&payload, sst.identity.checksum_algorithm);
            }
        }
        sstv2::codec::append_fixed32(&payload,
                                     static_cast<uint32_t>(manifest.installed_edits.size()));
        for (const auto& edit : manifest.installed_edits) {
            sstv2::codec::append_fixed64(&payload, edit.edit_id);
            sstv2::codec::append_fixed64(&payload, edit.result_generation);
            sstv2::codec::append_fixed64(&payload, edit.semantic_fingerprint);
        }
        std::string encoded(kMagic);
        sstv2::codec::append_fixed64(&encoded, payload.size());
        encoded.append(payload);
        sstv2::codec::append_fixed64(&encoded, sstv2::codec::crc32c_u64(payload));
        if (encoded.size() > kMaxManifestBytes) {
            return absl::ResourceExhaustedError("manifest exceeds size limit");
        }
        return encoded;
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("manifest encoding allocation failed");
    }
}

absl::StatusOr<std::shared_ptr<const ManifestRef>> DecodeManifest(
    std::span<const std::byte> encoded) {
    if (encoded.size() < kMagic.size() + 16 || encoded.size() > kMaxManifestBytes) {
        return absl::DataLossError("invalid manifest size");
    }
    const std::string_view bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    if (!bytes.starts_with(kMagic)) {
        return absl::DataLossError("invalid manifest magic");
    }
    const uint64_t payload_length = sstv2::codec::read_fixed64(bytes, kMagic.size());
    const size_t header_size = kMagic.size() + sizeof(uint64_t);
    if (payload_length != bytes.size() - header_size - sizeof(uint64_t)) {
        return absl::DataLossError("invalid manifest payload length");
    }
    const auto payload = bytes.substr(header_size, static_cast<size_t>(payload_length));
    const uint64_t expected_checksum =
        sstv2::codec::read_fixed64(bytes, header_size + static_cast<size_t>(payload_length));
    if (sstv2::codec::crc32c_u64(payload) != expected_checksum) {
        return absl::DataLossError("manifest checksum mismatch");
    }

    try {
        Decoder decoder(payload);
        auto manifest = std::make_shared<ManifestRef>();
        auto format_version = decoder.u32();
        auto generation = decoder.u64();
        auto next_sequence = decoder.u64();
        auto key_format = decoder.u64();
        auto schema_fingerprint = decoder.u64();
        auto partition_mode = decoder.u64();
        auto hash_algorithm = decoder.u64();
        auto virtual_buckets = decoder.u64();
        auto domain_fingerprint = decoder.u64();
        auto timestamp_domain = decoder.u64();
        auto timestamp_watermark = decoder.u64();
        auto last_commit_physical_ms = decoder.u64();
        auto group_count = decoder.u32();
        if (!format_version.ok() || !generation.ok() || !next_sequence.ok() || !key_format.ok() ||
            !schema_fingerprint.ok() || !partition_mode.ok() || !hash_algorithm.ok() ||
            !virtual_buckets.ok() || !domain_fingerprint.ok() || !timestamp_domain.ok() ||
            !timestamp_watermark.ok() || !last_commit_physical_ms.ok() || !group_count.ok() ||
            *group_count == 0 || *group_count > kMaxLocalityGroups) {
            return absl::DataLossError("invalid manifest header");
        }
        manifest->format_version = *format_version;
        manifest->generation = *generation;
        manifest->next_sst_sequence = *next_sequence;
        manifest->comparator_domain = {.key_format_version = *key_format,
                                       .row_key_schema_fingerprint = *schema_fingerprint,
                                       .partition_mode = *partition_mode,
                                       .hash_algorithm_version = *hash_algorithm,
                                       .virtual_bucket_count = *virtual_buckets,
                                       .fingerprint = *domain_fingerprint};
        manifest->timestamp_domain_epoch = *timestamp_domain;
        manifest->timestamp_high_watermark = *timestamp_watermark;
        manifest->last_commit_physical_ms = *last_commit_physical_ms;
        for (uint32_t i = 0; i < *group_count; ++i) {
            auto id = decoder.u32();
            auto flushed = decoder.u64();
            auto sst_count = decoder.u32();
            if (!id.ok() || !flushed.ok() || !sst_count.ok() ||
                *sst_count > kMaxSstsPerLocalityGroup) {
                return absl::DataLossError("invalid manifest locality group");
            }
            LocalityGroupManifest group{.flushed_applied_index = *flushed};
            group.ssts.reserve(*sst_count);
            for (uint32_t j = 0; j < *sst_count; ++j) {
                auto sequence = decoder.u64();
                auto key_path = decoder.string();
                auto value_path = decoder.string();
                auto key_file = decoder.identity();
                auto value_file = decoder.identity();
                auto row_count = decoder.u64();
                auto sst_format = decoder.u64();
                auto sst_key_format = decoder.u64();
                auto sst_schema_fingerprint = decoder.u64();
                auto sst_partition_mode = decoder.u64();
                auto sst_hash_algorithm = decoder.u64();
                auto sst_virtual_buckets = decoder.u64();
                auto sst_domain_fingerprint = decoder.u64();
                auto checksum_algorithm = decoder.u64();
                if (!sequence.ok() || !key_path.ok() || !value_path.ok() || !key_file.ok() ||
                    !value_file.ok() || !row_count.ok() || !sst_format.ok() ||
                    !sst_key_format.ok() || !sst_schema_fingerprint.ok() ||
                    !sst_partition_mode.ok() || !sst_hash_algorithm.ok() ||
                    !sst_virtual_buckets.ok() || !sst_domain_fingerprint.ok() ||
                    !checksum_algorithm.ok()) {
                    return absl::DataLossError("invalid manifest SST entry");
                }
                group.ssts.push_back(
                    {.sequence = *sequence,
                     .identity = {
                         .key_path = std::move(*key_path),
                         .value_path = std::move(*value_path),
                         .key_file = *key_file,
                         .value_file = *value_file,
                         .row_count = *row_count,
                         .sst_format_version = *sst_format,
                         .comparator_domain = {
                             .key_format_version = *sst_key_format,
                             .row_key_schema_fingerprint = *sst_schema_fingerprint,
                             .partition_mode = *sst_partition_mode,
                             .hash_algorithm_version = *sst_hash_algorithm,
                             .virtual_bucket_count = *sst_virtual_buckets,
                             .fingerprint = *sst_domain_fingerprint},
                         .checksum_algorithm = *checksum_algorithm}});
            }
            if (!manifest->locality_groups.emplace(*id, std::move(group)).second) {
                return absl::DataLossError("duplicate manifest locality group");
            }
        }
        auto ledger_count = decoder.u32();
        if (!ledger_count.ok() || *ledger_count > kMaxInstalledEditLedgerEntries) {
            return absl::DataLossError("invalid manifest ledger size");
        }
        manifest->installed_edits.reserve(*ledger_count);
        for (uint32_t i = 0; i < *ledger_count; ++i) {
            auto edit_id = decoder.u64();
            auto result_generation = decoder.u64();
            auto semantic_fingerprint = decoder.u64();
            if (!edit_id.ok() || !result_generation.ok() || !semantic_fingerprint.ok()) {
                return absl::DataLossError("invalid manifest ledger entry");
            }
            manifest->installed_edits.push_back({.edit_id = *edit_id,
                                                 .result_generation = *result_generation,
                                                 .semantic_fingerprint = *semantic_fingerprint});
        }
        if (decoder.remaining() != 0) {
            return absl::DataLossError("manifest has trailing payload bytes");
        }
        auto status = ValidateManifest(*manifest);
        if (!status.ok()) {
            return absl::DataLossError(status.message());
        }
        return std::shared_ptr<const ManifestRef>(std::move(manifest));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("manifest decoding allocation failed");
    }
}

absl::StatusOr<PersistedManifest> PersistManifest(
    const std::shared_ptr<sstv2::io::FileSystem>& filesystem,
    std::string path,
    const ManifestRef& manifest) {
    if (filesystem == nullptr || path.empty()) {
        return absl::InvalidArgumentError("invalid manifest persistence target");
    }
    auto encoded = EncodeManifest(manifest);
    if (!encoded.ok()) {
        return encoded.status();
    }
    const std::string pending_path = path + ".pending";
    auto handle = filesystem->create(pending_path, {.overwrite = true});
    if (!handle.ok()) {
        return handle.status();
    }
    auto status = filesystem->append(*handle, std::as_bytes(std::span(*encoded)));
    if (!status.ok()) {
        auto identity = filesystem->close(*handle);
        if (identity.ok()) {
            (void)filesystem->remove(pending_path, *identity);
        } else {
            (void)filesystem->remove(pending_path);
        }
        return status;
    }
    auto identity = filesystem->close(*handle);
    if (!identity.ok()) {
        (void)filesystem->remove(pending_path);
        return identity.status();
    }
    status = filesystem->rename(pending_path, path);
    if (!status.ok()) {
        (void)filesystem->remove(pending_path, *identity);
        return status;
    }
    return PersistedManifest{
        .path = std::move(path), .identity = *identity, .generation = manifest.generation};
}

absl::StatusOr<std::shared_ptr<const ManifestRef>> LoadManifest(
    const std::shared_ptr<sstv2::io::FileSystem>& filesystem, const PersistedManifest& persisted) {
    if (filesystem == nullptr || persisted.path.empty() || persisted.identity.file_id == 0 ||
        !persisted.identity.checksum_valid || persisted.identity.length > kMaxManifestBytes) {
        return absl::InvalidArgumentError("invalid persisted manifest identity");
    }
    auto handle = filesystem->open(persisted.path, persisted.identity);
    if (!handle.ok()) {
        return handle.status();
    }
    std::vector<std::byte> bytes;
    try {
        bytes.resize(static_cast<size_t>(persisted.identity.length));
    } catch (const std::bad_alloc&) {
        (void)filesystem->close(*handle);
        return absl::ResourceExhaustedError("manifest read buffer allocation failed");
    }
    auto status = filesystem->read_at(*handle, 0, bytes);
    auto close_status = filesystem->close(*handle);
    if (!status.ok()) {
        return status;
    }
    if (!close_status.ok()) {
        return close_status.status();
    }
    auto manifest = DecodeManifest(bytes);
    if (!manifest.ok()) {
        return manifest.status();
    }
    if ((*manifest)->generation != persisted.generation) {
        return absl::FailedPreconditionError("persisted manifest generation mismatch");
    }
    return manifest;
}

} // namespace pl::minitable
