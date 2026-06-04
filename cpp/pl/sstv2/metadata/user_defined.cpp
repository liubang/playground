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

#include "cpp/pl/sstv2/metadata/user_defined.h"

namespace pl::sstv2::metadata {

std::string UserDefined::serialize() const {
    return section.serialize(kUserDefinedMagic);
}

absl::StatusOr<UserDefined> UserDefined::deserialize(std::span<const std::byte> data) {
    auto section_or = MetadataSection::deserialize(data, kUserDefinedMagic);
    if (!section_or.ok()) {
        return section_or.status();
    }
    UserDefined ud;
    ud.section = std::move(*section_or);
    return ud;
}

} // namespace pl::sstv2::metadata
