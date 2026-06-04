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

#include "cpp/pl/sstv2/metadata/configuration.h"

#include "cpp/pl/sstv2/metadata/metadata_section.h"

namespace pl::sstv2::metadata {

namespace {
constexpr std::string_view kKeyBlockSize = "block_size";
constexpr std::string_view kKeyCompression = "compression";
constexpr std::string_view kKeyBloomBitsPerKey = "bloom_bits_per_key";
constexpr std::string_view kKeyValueSizeThreshold = "value_size_threshold";
constexpr std::string_view kKeyMaxPrefixRounds = "max_prefix_rounds";
} // namespace

std::string Configuration::serialize() const {
    MetadataSection section;
    section.put_uint32(kKeyBlockSize, block_size);
    section.put_uint16(kKeyCompression, static_cast<uint16_t>(compression));
    section.put_uint32(kKeyBloomBitsPerKey, bloom_bits_per_key);
    section.put_uint32(kKeyValueSizeThreshold, value_size_threshold);
    section.put_uint16(kKeyMaxPrefixRounds, static_cast<uint16_t>(max_prefix_rounds));
    return section.serialize(kConfigurationMagic);
}

absl::StatusOr<Configuration> Configuration::deserialize(std::span<const std::byte> data) {
    auto section_or = MetadataSection::deserialize(data, kConfigurationMagic);
    if (!section_or.ok()) {
        return section_or.status();
    }
    auto& section = *section_or;

    Configuration config;
    if (auto v = section.get_uint32(kKeyBlockSize)) {
        config.block_size = *v;
    }
    if (auto v = section.get_uint16(kKeyCompression)) {
        config.compression = static_cast<uint8_t>(*v);
    }
    if (auto v = section.get_uint32(kKeyBloomBitsPerKey)) {
        config.bloom_bits_per_key = *v;
    }
    if (auto v = section.get_uint32(kKeyValueSizeThreshold)) {
        config.value_size_threshold = *v;
    }
    if (auto v = section.get_uint16(kKeyMaxPrefixRounds)) {
        config.max_prefix_rounds = static_cast<uint8_t>(*v);
    }
    return config;
}

} // namespace pl::sstv2::metadata
