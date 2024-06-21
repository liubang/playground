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

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

namespace pl {
class Binary {
public:
    Binary() : data_(""), size_(0) {}
    Binary(const char* d, size_t n) : data_(d), size_(n) {}
    Binary(const std::string& s) : data_(s.data()), size_(s.size()) {}
    Binary(const char* s) : data_(s), size_(strlen(s)) {}

    Binary(const Binary&) = default;

    Binary& operator=(const Binary&) = default;

    void reset(const std::string& s) {
        data_ = s.data();
        size_ = s.size();
    }

    void reset(const char* d, std::size_t n) {
        data_ = d;
        size_ = n;
    }

    [[nodiscard]] const char* data() const { return data_; }

    [[nodiscard]] std::size_t size() const { return size_; }

    [[nodiscard]] bool empty() const { return size_ == 0; }

    char operator[](std::size_t n) const {
        assert(n < size_);
        return data_[n];
    }

    void clear() {
        data_ = "";
        size_ = 0;
    }

    [[nodiscard]] std::string toString() const { return {data_, size_}; }

    [[nodiscard]] int compare(const Binary& b) const;

private:
    const char* data_;
    std::size_t size_;
};

inline bool operator==(const Binary& a, const Binary& b) {
    return (a.size() == b.size()) && (::memcmp(a.data(), b.data(), a.size()) == 0);
}

inline int Binary::compare(const Binary& b) const {
    std::size_t min_len = std::min(size_, b.size());
    int r = ::memcmp(data_, b.data(), min_len);
    if (r == 0) {
        if (size_ > b.size()) {
            return 1;
        }
        if (size_ < b.size()) {
            return -1;
        }
    }
    return r;
}

} // namespace pl
