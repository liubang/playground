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

#include <exception>
#include <initializer_list>

namespace pl {

struct nullopt_t {
    explicit nullopt_t() = default;
};

inline constexpr nullopt_t nullopt;

struct inplace_t {
    explicit inplace_t() = default;
};

inline constexpr inplace_t inplace;

class IllegalStateException : public std::exception {};

template <typename T> struct Optional {
    Optional() : has_value_(false) {}
    Optional(T&& value) : has_value_(true), value_(std::move(value)) {}
    Optional(const T& value) : has_value_(true), value_(value) {}
    Optional(nullopt_t) : has_value_(false) {}
    Optional(const Optional& that) : has_value_(that.has_value_) {
        if (has_value_) {
            new (&value_) T(that.value_);
        }
    }
    Optional(Optional&& that) noexcept : has_value_(that.has_value_) {
        if (has_value_) {
            new (&value_) T(std::move(that.value_));
            that.has_value_ = false;
        }
    }
    template <typename... Ts>
    explicit Optional(inplace_t, Ts&&... args)
        : has_value_(true), value_(std::forward<Ts>(args)...) {}

    template <typename U, typename... Ts>
    explicit Optional(inplace_t, std::initializer_list<U> list, Ts&&... args)
        : has_value_(true), value_(list, std::forward<Ts>(args)...) {}

    Optional& operator=(nullopt_t) {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        return *this;
    }

    Optional& operator=(T&& value) {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        new (&value_) T(std::move(value));
        has_value_ = true;
        return *this;
    }

    Optional& operator=(const T& value) {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        new (&value_) T(value);
        has_value_ = true;
        return *this;
    }

    Optional& operator=(Optional&& that) noexcept {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        if (that.has_value_) {
            new (&value_) T(std::move(that.value_));
            that.value_.~T();
        }
        has_value_ = that.has_value_;
        that.has_value_ = false;
        return *this;
    }

    Optional& operator=(const Optional& that) noexcept {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        if (that.has_value_) {
            new (&value_) T(that.value_);
        }
        has_value_ = that.has_value_;
        return *this;
    }

    ~Optional() {
        if (has_value_) {
            value_.~T();
        }
    }

    template <typename... Ts> void emplace(Ts&&... args) {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        new (&value_) T(std::forward<Ts>(args)...);
        has_value_ = true;
    }

    template <typename U, typename... Ts>
    void emplace(std::initializer_list<U> list, Ts&&... args) {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
        new (&value_) T(list, std::forward<Ts>(args)...);
        has_value_ = true;
    }

    [[nodiscard]] bool has_value() const { return has_value_; }

    explicit operator bool() const noexcept { return has_value_; }

    bool operator==(nullopt_t) const noexcept { return !has_value_; }

    const T& value() const& {
        if (!has_value_) {
            throw new IllegalStateException;
        }
        return value_;
    }

    T& value() & {
        if (!has_value_) {
            throw new IllegalStateException;
        }
        return value_;
    }

    const T&& value() const&& {
        if (!has_value_) {
            throw new IllegalStateException;
        }
        return std::move(value_);
    }

    T&& value() && {
        if (!has_value_) {
            throw new IllegalStateException;
        }
        return std::move(value_);
    }

    const T& value_or(T&& default_value) const& {
        if (!has_value_) {
            return default_value;
        }
        return value_;
    }

    T&& value_or(T&& default_value) && {
        if (!has_value_) {
            return default_value;
        }
        return std::move(value_);
    }

    const T& operator*() const& noexcept { return value_; }

    T& operator*() & noexcept { return value_; }

    T&& operator*() && noexcept { return std::move(value_); }

    const T* operator->() const noexcept { return &value_; }

    T* operator->() noexcept { return &value_; }

private:
    bool has_value_;
    union {
        T value_;
    };
};

} // namespace pl
