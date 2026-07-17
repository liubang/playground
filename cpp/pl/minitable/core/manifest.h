// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/minitable/core/sst_read_source.h"
#include "cpp/pl/sstv2/io/filesystem.h"

namespace pl::minitable {

inline constexpr uint32_t kManifestFormatVersion = 2;
inline constexpr size_t kMaxInstalledEditLedgerEntries = 64;

struct ManifestSst {
    uint64_t sequence = 0;
    SstIdentity identity;

    bool operator==(const ManifestSst&) const = default;
};

struct LocalityGroupManifest {
    uint64_t flushed_applied_index = 0;
    // Newest first.
    std::vector<ManifestSst> ssts;

    bool operator==(const LocalityGroupManifest&) const = default;
};

struct InstalledManifestEdit {
    uint64_t edit_id = 0;
    uint64_t result_generation = 0;
    uint64_t semantic_fingerprint = 0;

    bool operator==(const InstalledManifestEdit&) const = default;
};

// Immutable authoritative live-set object. Runtime read sources are deliberately
// outside the serialized model and are rebuilt with identity-fenced opens.
struct ManifestRef {
    uint32_t format_version = kManifestFormatVersion;
    uint64_t generation = 1;
    uint64_t next_sst_sequence = 1;
    std::map<uint32_t, LocalityGroupManifest> locality_groups;
    std::vector<InstalledManifestEdit> installed_edits;

    bool operator==(const ManifestRef&) const = default;
};

struct ManifestEdit {
    uint64_t edit_id = 0;
    uint32_t locality_group_id = 0;
    uint64_t parent_generation = 0;
    uint64_t new_generation = 0;
    uint64_t immutable_id = 0;
    uint64_t immutable_fence_index = 0;
    std::vector<SstIdentity> outputs;
};

struct PersistedManifest {
    std::string path;
    sstv2::io::FileIdentity identity;
    uint64_t generation = 0;
};

[[nodiscard]] absl::StatusOr<std::shared_ptr<const ManifestRef>> ApplyManifestEdit(
    const ManifestRef& current, const ManifestEdit& edit);
[[nodiscard]] absl::StatusOr<std::string> EncodeManifest(const ManifestRef& manifest);
[[nodiscard]] absl::StatusOr<std::shared_ptr<const ManifestRef>> DecodeManifest(
    std::span<const std::byte> encoded);
[[nodiscard]] absl::StatusOr<PersistedManifest> PersistManifest(
    const std::shared_ptr<sstv2::io::FileSystem>& filesystem,
    std::string path,
    const ManifestRef& manifest);
[[nodiscard]] absl::StatusOr<std::shared_ptr<const ManifestRef>> LoadManifest(
    const std::shared_ptr<sstv2::io::FileSystem>& filesystem, const PersistedManifest& persisted);

} // namespace pl::minitable
