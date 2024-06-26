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

#include <string>

namespace pl {

#define NEW_STATUS(name) \
    static Status New##name(const std::string& msg = "") { return {k##name, msg}; }

#define IS_STATUS(name) \
    [[nodiscard]] bool is##name() const { return code_ == k##name; }

class Status {
public:
    Status() = default;
    Status(const Status&) = default;

    Status& operator=(const Status&) = default;

    NEW_STATUS(Ok)
    NEW_STATUS(NotFound)
    NEW_STATUS(Corruption)
    NEW_STATUS(NotSupported)
    NEW_STATUS(InvalidArgument)
    NEW_STATUS(IOError)

    IS_STATUS(Ok)
    IS_STATUS(NotFound)
    IS_STATUS(Corruption)
    IS_STATUS(NotSupported)
    IS_STATUS(InvalidArgument)
    IS_STATUS(IOError)

    [[nodiscard]] const std::string& msg() const { return msg_; }

    [[nodiscard]] std::string to_string() const {
        switch (code_) {
        case kOk:
            return "OK";
        case kNotFound:
            return "NotFound";
        case kCorruption:
            return "Corruption";
        case kNotSupported:
            return "NotSupported";
        case kInvalidArgument:
            return "InvalidArgument";
        case kIOError:
            return "IOError";
        default:
            return "Unknown";
        }
    }

private:
    enum Code {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5
    };

    Status(Code code, std::string_view msg) : code_(code), msg_(msg) {}

private:
    Code code_{kOk};
    std::string msg_;
};

#undef IS_STATUS
#undef NEW_STATUS

} // namespace pl
