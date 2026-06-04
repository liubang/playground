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
// Created: 2026/06/04 15:23

#include "cpp/pl/sstv2/metadata/compatibility.h"

#include "cpp/pl/sstv2/metadata/metadata_section.h"

namespace pl::sstv2::metadata {

namespace {
constexpr std::string_view kKeyMinReaderVersion = "min_reader_version";
constexpr std::string_view kKeyWriterVersion = "writer_version";
constexpr std::string_view kKeyFeatureFlags = "feature_flags";
} // namespace

std::string Compatibility::serialize() const {
    MetadataSection section;
    section.put_uint16(kKeyMinReaderVersion, min_reader_version);
    section.put_uint16(kKeyWriterVersion, writer_version);
    section.put_uint64(kKeyFeatureFlags, feature_flags);
    return section.serialize(kCompatibilityMagic);
}

absl::StatusOr<Compatibility> Compatibility::deserialize(std::span<const std::byte> data) {
    auto section_or = MetadataSection::deserialize(data, kCompatibilityMagic);
    if (!section_or.ok()) {
        return section_or.status();
    }
    auto& section = *section_or;

    Compatibility compat;
    if (auto v = section.get_uint16(kKeyMinReaderVersion)) {
        compat.min_reader_version = *v;
    }
    if (auto v = section.get_uint16(kKeyWriterVersion)) {
        compat.writer_version = *v;
    }
    if (auto v = section.get_uint64(kKeyFeatureFlags)) {
        compat.feature_flags = *v;
    }
    return compat;
}

} // namespace pl::sstv2::metadata
