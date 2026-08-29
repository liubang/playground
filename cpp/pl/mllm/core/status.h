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
// Created: 2026/08/29 22:15

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <variant>

namespace pl::mllm {

enum class ErrorCode {
    kOk,
    kInvalidArgument,
    kNotFound,
    kInvalidFormat,
    kUnsupported,
    kOutOfMemory,
    kBackendFailure,
    kCancelled,
    kInternal,
};

struct Status {
    ErrorCode code = ErrorCode::kOk;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }

    static Status Error(ErrorCode code, std::string message) {
        return Status{code, std::move(message)};
    }

    bool operator==(const Status&) const = default;
};

template <typename T> class Result {
public:
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(Status status) : data_(std::in_place_index<1>, std::move(status)) {
        if (std::get<1>(data_).ok()) { // an "ok" Status must never erase the value channel
            std::get<1>(data_) =
                Status::Error(ErrorCode::kInternal, "Result constructed from ok Status");
        }
    }

    [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }

    [[nodiscard]] const Status& status() const noexcept {
        static const Status kOkStatus{};
        return ok() ? kOkStatus : std::get<1>(data_);
    }

    T& value() & {
        require_ok();
        return std::get<0>(data_);
    }
    const T& value() const& {
        require_ok();
        return std::get<0>(data_);
    }
    T&& value() && {
        require_ok();
        return std::move(std::get<0>(data_));
    }

private:
    void require_ok() const noexcept { // no exceptions: contract violation aborts
        if (!ok()) {
            std::fprintf(
                stderr, "mllm: Result::value() on error: %s\n", std::get<1>(data_).message.c_str());
            std::abort();
        }
    }

    std::variant<T, Status> data_;
};

} // namespace pl::mllm
