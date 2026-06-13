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
// Created: 2026/06/06 14:16

#pragma once

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

[[nodiscard]] absl::Status encode_value(const Value& value, std::string* dst);
[[nodiscard]] absl::StatusOr<std::string> encode_value(const Value& value);
[[nodiscard]] absl::StatusOr<Value> decode_value(DataType type, std::string_view bytes);

} // namespace pl::sstv2::types
