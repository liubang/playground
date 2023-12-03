// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "cpp/meta/traits.h"

#include <cstdio>
#include <type_traits>
#include <utility>

namespace pl {

template <typename T> struct DefaultDeleter {
    void operator()(T* p) const { delete p; }
};

template <typename T> struct DefaultDeleter<T[]> {
    void operator()(T* p) const { delete[] p; }
};

template <> struct DefaultDeleter<FILE> {
    void operator()(FILE* p) const { fclose(p); }
};

template <typename T, typename Deleter = DefaultDeleter<T>> struct unique_ptr {
public:
    unique_ptr(std::nullptr_t dummy = nullptr) : p_(dummy) {}
    explicit unique_ptr(T* p) : p_(p) {}

    unique_ptr(const unique_ptr& that) = delete;

    unique_ptr& operator=(const unique_ptr& that) = delete;

    unique_ptr(unique_ptr&& that) noexcept { p_ = std::exchange(that.p_, nullptr); }

    unique_ptr& operator=(unique_ptr&& that) noexcept {
        if (this != &that) [[likely]] {
            if (p_) {
                Deleter{}(p_);
            }
            p_ = std::exchange(that.p_, nullptr);
        }
        return *this;
    }

    template <typename U, typename D, typename = enable_if_t<std::is_convertible_v<U*, T*>>>
    unique_ptr(unique_ptr<U, D>&& that) {
        p_ = std::exchange(that.p_, nullptr);
    }

    ~unique_ptr() {
        if (p_) {
            Deleter{}(p_);
        }
    }

    T* get() const { return p_; }

    T* release() { return std::exchange(p_, nullptr); }

    void reset(T* p = nullptr) {
        if (p_) {
            Deleter{}(p_);
        }
        p_ = p;
    }

    T& operator*() const { return *p_; }

    T* operator->() const { return p_; }

private:
    T* p_;

    template <typename U, typename D> friend struct unique_ptr;
};

template <typename T, typename Deleter> struct unique_ptr<T[], Deleter> : unique_ptr<T, Deleter> {};

template <typename T, typename... Args> unique_ptr<T> make_unique(Args&&... args) {
    return unique_ptr<T>(new T(std::forward<Args>(args)...));
}

} // namespace pl
