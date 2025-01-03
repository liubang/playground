// Copyright (c) 2024 The Authors. All rights reserved.
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

#include <memory>
#include <string>

namespace pl {

// clang-format off
enum class Code : uint32_t {
    ST_Ok              = 0,
    ST_NotFound        = 1,
    ST_Corruption      = 2,
    ST_NotSupported    = 3,
    ST_InvalidArgument = 4,
    ST_IOError         = 5
};
// clang-format on

#define __PL_CONCAT(a, b) a##b
#define __PL_ENUM(code)   __PL_CONCAT(Code::ST_, code)
#define __PL_NEW_STATUS_FUNC(code) \
    static Status New##code(const std::string& msg = "") { return {__PL_ENUM(code), msg}; }
#define __PL_IS_STATUS_FUNC(code) \
    [[nodiscard]] bool is##code() const { return code_ == __PL_ENUM(code); }

class Status {
public:
    Status() = default;
    // default copy and move
    Status(const Status&) = default;
    Status(Status&&) = default;
    Status& operator=(const Status&) = default;
    Status& operator=(Status&&) = default;

    [[nodiscard]] bool ok() const { return code_ == Code::ST_Ok; }
    [[nodiscard]] Code code() const { return code_; }

    __PL_NEW_STATUS_FUNC(Ok)
    __PL_NEW_STATUS_FUNC(NotFound)
    __PL_NEW_STATUS_FUNC(Corruption)
    __PL_NEW_STATUS_FUNC(NotSupported)
    __PL_NEW_STATUS_FUNC(InvalidArgument)
    __PL_NEW_STATUS_FUNC(IOError)

    __PL_IS_STATUS_FUNC(Ok)
    __PL_IS_STATUS_FUNC(NotFound)
    __PL_IS_STATUS_FUNC(Corruption)
    __PL_IS_STATUS_FUNC(NotSupported)
    __PL_IS_STATUS_FUNC(InvalidArgument)
    __PL_IS_STATUS_FUNC(IOError)

    [[nodiscard]] const std::string_view msg() const {
        if (msg_ != nullptr) {
            return {msg_->data(), msg_->size()};
        }
        return std::string_view();
    }

private:
    Status(Code code, std::string_view msg) : code_(code) {
        if (!msg.empty()) {
            msg_ = std::make_shared<std::string>(msg);
        }
    }

private:
    Code code_{Code::ST_Ok};
    std::shared_ptr<std::string> msg_;
};

} // namespace pl
