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

#pragma once

#include "cpp/pl/status/status.h"
#include "cpp/pl/status/status_code.h"

#include <type_traits>

#include <folly/Expected.h>
#include <folly/logging/xlog.h>

#define RETURN_ERROR(result) return pl::makeError(std::move(result.error()))
#define RETURN_VOID \
    return pl::Void {}

#define MAKE_ERROR_F(code, ...) pl::makeError((code), fmt::format(__VA_ARGS__))

#define RETURN_AND_LOG_ON_ERROR(result)               \
    do {                                              \
        auto&& _result = (result);                    \
        if (UNLIKELY(_result.hasError())) {           \
            XLOGF(ERR, "error: {}", _result.error()); \
            RETURN_ERROR(_result);                    \
        }                                             \
    } while (0)

namespace pl {

template <typename T> using Result = folly::Expected<T, Status>;

template <typename T> struct IsResult : std::false_type {};

template <typename T> struct IsResult<Result<T>> : std::true_type {};

using Void = folly::Unit;

template <typename... Args>
[[nodiscard]] inline folly::Unexpected<Status> makeError(Args&&... args) {
    return folly::makeUnexpected(Status(std::forward<Args>(args)...));
}

template <typename T> [[nodiscard]] inline status_code_t getStatusCode(const Result<T>& result) {
    return result.hasError() ? result.error().code() : StatusCode::kOK;
}

} // namespace pl
