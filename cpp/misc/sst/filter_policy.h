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

#include "cpp/tools/binary.h"

#include <memory>
#include <string>

namespace pl {

class FilterPolicy {
public:
    virtual ~FilterPolicy() = default;

    /**
     * @brief description]
     *
     * @param keys parameter]
     * @param n parameter]
     * @param dst parameter]
     */
    virtual void createFilter(Binary* keys, std::size_t n, std::string* dst) const = 0;

    /**
     * @brief description]
     *
     * @return return]
     */
    [[nodiscard]] virtual const char* name() const = 0;

    /**
     * @brief description]
     *
     * @param key parameter]
     * @param filter parameter]
     * @return return]
     */
    [[nodiscard]] virtual bool keyMayMatch(const Binary& key, const Binary& filter) const = 0;
};

using FilterPolicyPtr = std::unique_ptr<FilterPolicy>;
using FilterPolicyRef = std::shared_ptr<FilterPolicy>;

/**
 * @class BloomFilterPolicy
 * @brief this class is a proxy of bloom::BloomFilter
 */
class BloomFilterPolicy : public FilterPolicy {
public:
    BloomFilterPolicy(std::size_t bits_per_key) : bits_per_key_(bits_per_key) {}

    ~BloomFilterPolicy() override = default;

    [[nodiscard]] const char* name() const override { return "BloomFilterPolicy"; }

    void createFilter(Binary* keys, std::size_t n, std::string* dst) const override;

    [[nodiscard]] bool keyMayMatch(const Binary& key, const Binary& filter) const override;

private:
    std::size_t bits_per_key_;
};

} // namespace pl
