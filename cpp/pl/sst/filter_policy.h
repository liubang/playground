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

#include "cpp/pl/bloom/bloom.h"

#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pl {

class FilterBuilder {
public:
    virtual ~FilterBuilder() = default;

    virtual void add_key(std::string_view key);

    virtual void add_key_alt(std::string_view key, std::string_view prefix);

    virtual std::string_view finish(std::unique_ptr<const char[]>* buf) = 0;

protected:
    std::deque<uint64_t> hashes_;
};

using FilterBuilderPtr = std::unique_ptr<FilterBuilder>;
using FilterBuilderRef = std::shared_ptr<FilterBuilder>;

class StandardBloomFilterBuilder : public FilterBuilder {

public:
    StandardBloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}

    ~StandardBloomFilterBuilder() override = default;

    std::string_view finish(std::unique_ptr<const char[]>* buf) override;

private:
    [[nodiscard]] std::size_t calculate_space(std::size_t num_hashes) const;

    void add_all_hashes(char* buf, std::size_t buf_len, int num_probes);

private:
    int bits_per_key_{0};
};

class BlockedBloomFilterBuilder : public FilterBuilder {

public:
    BlockedBloomFilterBuilder(int millibits_per_key) : millibits_per_key_(millibits_per_key) {}

    ~BlockedBloomFilterBuilder() override = default;

    std::string_view finish(std::unique_ptr<const char[]>* buf) override;

private:
    void add_all_hashes(char* buf, std::size_t buf_len, int num_probes);

    int get_num_probes(std::size_t num_hashes, std::size_t length_with_metadata);

    [[nodiscard]] std::size_t calculate_space(std::size_t num_hashes) const;

private:
    int millibits_per_key_{0};
};

class FilterReader {
public:
    virtual ~FilterReader() = default;

    virtual bool key_may_match(std::string_view key) = 0;
};

using FilterReaderPtr = std::unique_ptr<FilterReader>;
using FilterReaderRef = std::shared_ptr<FilterReader>;

class StandardBloomFilterReader : public FilterReader {
public:
    StandardBloomFilterReader(const char* buf, uint32_t buf_length, int num_probes)
        : buf_(buf), buf_length_(buf_length), num_probes_(num_probes) {}

    ~StandardBloomFilterReader() override = default;

    bool key_may_match(std::string_view key) override;

private:
    const char* buf_{nullptr};
    const uint32_t buf_length_{0};
    const int num_probes_{0};
};

class BlockedBloomFilterReader : public FilterReader {
public:
    BlockedBloomFilterReader(const char* buf, uint32_t buf_length, int num_probes)
        : buf_(buf), buf_length_(buf_length), num_probes_(num_probes) {}

    ~BlockedBloomFilterReader() override = default;

    bool key_may_match(std::string_view key) override;

private:
    const char* buf_{nullptr};
    const uint32_t buf_length_{0};
    const int num_probes_{0};
};

//=================================================================================================
// @deprecated
class FilterPolicy;

// @deprecated
using FilterPolicyPtr = std::unique_ptr<FilterPolicy>;
// @deprecated
using FilterPolicyRef = std::shared_ptr<FilterPolicy>;

/**
 * @deprecated
 */
class FilterPolicy {
public:
    virtual ~FilterPolicy() = default;

    virtual void createFilter(const std::vector<std::string_view>& keys,
                              std::string* dst) const = 0;

    [[nodiscard]] virtual const char* name() const = 0;

    [[nodiscard]] virtual bool keyMayMatch(std::string_view key, std::string_view filter) const = 0;
};

/**
 * @deprecated
 */
class BloomFilterPolicy : public FilterPolicy {
public:
    BloomFilterPolicy(uint32_t bits_per_key) : bits_per_key_(bits_per_key) {}

    ~BloomFilterPolicy() override = default;

    [[nodiscard]] const char* name() const override { return "BloomFilterPolicy"; }

    void createFilter(const std::vector<std::string_view>& keys, std::string* dst) const override;

    [[nodiscard]] bool keyMayMatch(std::string_view key, std::string_view filter) const override;

private:
    uint32_t bits_per_key_;
};

} // namespace pl
