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
#include <optional>
#include <string_view>

namespace pl {

class FilterBuilder {
public:
    virtual ~FilterBuilder() = default;

    virtual void add_key(std::string_view key);

    virtual void add_key_and_alt(std::string_view key, std::string_view alt);

    virtual std::string_view finish(std::unique_ptr<const char[]>* buf) = 0;

    virtual size_t estimate_hashes_added();

    virtual const char* name() = 0;

protected:
    std::deque<uint64_t> hashes_;
    std::optional<uint64_t> prev_alt_hash_;
};

using FilterBuilderPtr = std::unique_ptr<FilterBuilder>;
using FilterBuilderRef = std::shared_ptr<FilterBuilder>;

class StandardBloomFilterBuilder : public FilterBuilder {

public:
    StandardBloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}

    ~StandardBloomFilterBuilder() override = default;

    std::string_view finish(std::unique_ptr<const char[]>* buf) override;

    const char* name() override { return kClassName(); }

    static const char* kClassName() { return "StandardBloomFilterBuilder"; }

private:
    [[nodiscard]] std::size_t calculate_space(std::size_t num_hashes) const;

    void add_all_hashes(char* buf, std::size_t buf_len, int num_probes);

private:
    int bits_per_key_{0};
};

class BlockedBloomFilterBuilder : public FilterBuilder {

public:
    BlockedBloomFilterBuilder(int bits_per_key) {
        millibits_per_key_ = static_cast<int>(bits_per_key * 1000.0 + 0.500001);
    }

    ~BlockedBloomFilterBuilder() override = default;

    std::string_view finish(std::unique_ptr<const char[]>* buf) override;

    const char* name() override { return kClassName(); }

    static const char* kClassName() { return "BlockedBloomFilterBuilder"; }

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

} // namespace pl
