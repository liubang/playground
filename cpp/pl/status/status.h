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

#include "status_code.h"

#include <any>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>

namespace pl {

class [[nodiscard]] Status {
    struct status_ok_t {};

public:
    Status() = delete;
    explicit Status(status_code_t code) : data_(construct(code, nullptr)) {}

    Status(const Status& other) { *this = other; }
    Status(Status&& other) = default;

    constexpr static status_ok_t OK{};
    Status(status_ok_t) : Status(StatusCode::kOK) {}

    Status(status_code_t code, std::string_view msg) {
        auto rep = std::make_unique<StatusRep>();
        rep->message = msg;
        data_ = construct(code, std::move(rep));
    }

    Status(status_code_t code, std::string&& msg) {
        auto rep = std::make_unique<StatusRep>();
        rep->message = std::move(msg);
        data_ = construct(code, std::move(rep));
    }

    Status(status_code_t code, const char* msg) : Status(code, std::string_view(msg)) {}

    template <typename T>
    Status(status_code_t code, std::string_view msg, T&& payload) : Status(code, msg) {
        setPayload(std::forward<T>(payload));
    }

    Status& operator=(const Status& other) {
        if (std::addressof(other) != this) {
            data_ = construct(other.code(), (other.rep() != nullptr)
                                                ? std::make_unique<StatusRep>(*other.rep())
                                                : nullptr);
        }
        return *this;
    }
    Status& operator=(Status&& other) = default;

    [[nodiscard]] std::string describe() const {
        return (rep() != nullptr)
                   ? fmt::format("{}({}) {}", StatusCode::toString(code()), code(), rep()->message)
                   : fmt::format("{}({})", StatusCode::toString(code()), code());
    }

    std::ostream& operator=(std::ostream& os) const { return os << describe(); }

    [[nodiscard]] status_code_t code() const {
        return reinterpret_cast<uintptr_t>(data_.get()) >> kPtrBits;
    }

    [[nodiscard]] std::string_view message() const {
        return (rep() != nullptr) ? std::string_view(rep()->message) : std::string_view{};
    }

    [[nodiscard]] bool isOk() const { return code() == StatusCode::kOK; }
    explicit operator bool() const { return isOk(); }

    template <typename T> T* payload() { return std::any_cast<T>(&rep()->payload); }
    template <typename T> const T* payload() const { return std::any_cast<T>(&rep()->payload); }

    [[nodiscard]] bool hasPayload() const {
        return (rep() != nullptr) && rep()->payload.has_value();
    }

    template <typename T> void setPayload(T&& payload) {
        ::printf("xxxxxxxxxxxxxxxxxxx\n");
        ensuredRep()->payload = std::forward<T>(payload);
    }

    template <typename T, typename... Args> void emplacePayload(Args&&... args) {
        ensuredRep()->payload.emplace<T>(std::forward<Args>(args)...);
    }

    void resetPayload() { (rep() != nullptr) ? rep()->payload.reset() : void(); }

private:
    static constexpr auto kPtrBits = 48u;
    static constexpr auto kPtrMask = ((1ul << kPtrBits) - 1);

    struct StatusRep {
        std::string message;
        std::any payload;
    };

    struct StatusRepDeleter {
        void operator()(StatusRep* rep) { delete extractPtr(rep); }
    };

    using StatusPtr = std::unique_ptr<StatusRep, StatusRepDeleter>;

    static StatusPtr construct(status_code_t code, std::unique_ptr<StatusRep> rep) {
        return StatusPtr(reinterpret_cast<StatusRep*>(reinterpret_cast<uintptr_t>(rep.release()) |
                                                      (uintptr_t(code) << kPtrBits)));
    }

    static StatusRep* extractPtr(StatusRep* rep) {
        return reinterpret_cast<StatusRep*>(reinterpret_cast<uintptr_t>(rep) & kPtrMask);
    }

    StatusRep* rep() { return extractPtr(data_.get()); }
    [[nodiscard]] const StatusRep* rep() const { return extractPtr(data_.get()); }

    StatusRep* ensuredRep() {
        if (rep() == nullptr) {
            data_ = construct(code(), std::make_unique<StatusRep>());
        }
        return rep();
    }

private:
    StatusPtr data_;
};

} // namespace pl

FMT_BEGIN_NAMESPACE
template <> struct formatter<pl::Status> : formatter<pl::status_code_t> {
    template <typename FormatContext>
    auto format(const pl::Status& status, FormatContext& ctx) const {
        auto msg = status.message();
        if (msg.empty()) {
            return fmt::format_to(ctx.out(), "{}({})", pl::StatusCode::toString(status.code()),
                                  status.code());
        }
        return fmt::format_to(ctx.out(), "{}({}) {}", pl::StatusCode::toString(status.code()),
                              status.code(), msg);
    }
};
FMT_END_NAMESPACE
