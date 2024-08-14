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
#include <string_view>

namespace pl {

class Comparator {
public:
    virtual ~Comparator() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    [[nodiscard]] virtual int compare(std::string_view a, std::string_view b) const = 0;

    virtual void findShortestSeparator(std::string* start, std::string_view limit) const = 0;

    virtual void findShortSucessor(std::string* key) const = 0;
};

using ComparatorPtr = std::unique_ptr<Comparator>;
using ComparatorRef = std::shared_ptr<Comparator>;

class BytewiseComparator : public Comparator {
public:
    BytewiseComparator() = default;

    [[nodiscard]] const char* name() const override { return "BytewiseComparator"; };

    [[nodiscard]] int compare(std::string_view a, std::string_view b) const override {
        return a.compare(b);
    }

    // TODO(liubang): use these method to reduce internal structure space usage
    void findShortestSeparator(std::string* start, std::string_view limit) const override {
        size_t min_length = std::min(start->size(), limit.size());
        size_t diff_index = 0;
        while ((diff_index < min_length) && ((*start)[diff_index] == limit[diff_index])) {
            ++diff_index;
        }
        if (diff_index == min_length) {
            // do nothing
            return;
        }
        auto diff_byte = static_cast<uint8_t>((*start)[diff_index]);
        // start: hello , limit: heln => helm
        if (diff_byte < static_cast<uint8_t>(0xff) &&
            diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
            (*start)[diff_index]++;
            start->resize(diff_index + 1);
        }
    }

    void findShortSucessor(std::string* key) const override {
        size_t n = key->size();
        for (size_t i = 0; i < n; ++i) {
            const auto byte = static_cast<uint8_t>((*key)[i]);
            if (byte != static_cast<uint8_t>(0xff)) {
                (*key)[i]++;
                key->resize(i + 1);
                return;
            }
        }
    }
};

} // namespace pl
