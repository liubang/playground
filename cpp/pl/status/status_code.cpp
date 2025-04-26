// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "status_code.h"

namespace pl::StatusCode {

std::string_view toString(status_code_t code) {
    switch (code) {
#define RAW_STATUS(name, ...) \
    case k##name:             \
        return #name;
#define STATUS(ns, name, ...) \
    case ns##Code::k##name:   \
        return #ns "::" #name;
#include "status_code_detail.h"
#undef RAW_STATUS
#undef STATUS
    };
    return "UnknownStatusCode";
}

StatusCodeType typeOf(status_code_t code) {
    switch (code) {
#define RAW_STATUS(name, ...) \
    case k##name:             \
        return StatusCodeType::Common;
#define STATUS(ns, name, ...) \
    case ns##Code::k##name:   \
        return StatusCodeType::ns;
#include "status_code_detail.h"
#undef RAW_STATUS
#undef STATUS
    };
    return StatusCodeType::Invalid;
}

} // namespace pl::StatusCode
