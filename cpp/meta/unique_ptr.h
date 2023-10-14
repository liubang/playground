//=====================================================================
//
// unique_ptr.h -
//
// Created by liubang on 2023/10/15 00:48
// Last Modified: 2023/10/15 00:48
//
//=====================================================================
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
