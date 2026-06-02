// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/06/02 22:23

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/execution/physical_executor.h"

namespace pl::flux::execution {
namespace {

constexpr size_t kDefaultQueryMemoryLimitBytes = size_t{256} * 1024 * 1024;

size_t parse_query_memory_limit() {
    const char* value = std::getenv("FLUX_QUERY_MAX_MEMORY_BYTES");
    if (value == nullptr || *value == '\0') {
        value = std::getenv("FLUX_ACCUMULATOR_MAX_BYTES");
    }
    if (value == nullptr || *value == '\0') {
        return kDefaultQueryMemoryLimitBytes;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed == 0) {
        return kDefaultQueryMemoryLimitBytes;
    }
    return static_cast<size_t>(parsed);
}

} // namespace

QueryMemoryContext::QueryMemoryContext(size_t limit_bytes) : limit_bytes_(limit_bytes) {}

std::shared_ptr<QueryMemoryContext> QueryMemoryContext::FromEnvironment() {
    return std::make_shared<QueryMemoryContext>(parse_query_memory_limit());
}

absl::Status QueryMemoryContext::Reserve(size_t bytes) {
    if (bytes == 0) {
        return absl::OkStatus();
    }
    std::scoped_lock lock(mu_);
    used_bytes_ += bytes;
    peak_bytes_ = std::max(peak_bytes_, used_bytes_);
    if (limit_bytes_ != 0 && used_bytes_ > limit_bytes_) {
        limited_ = true;
        return absl::ResourceExhaustedError(absl::StrCat("query memory limit exceeded: estimated=",
                                                         used_bytes_,
                                                         " bytes, limit=",
                                                         limit_bytes_,
                                                         " bytes"));
    }
    return absl::OkStatus();
}

void QueryMemoryContext::Release(size_t bytes) {
    if (bytes == 0) {
        return;
    }
    std::scoped_lock lock(mu_);
    used_bytes_ = bytes > used_bytes_ ? 0 : used_bytes_ - bytes;
}

MemoryProfile QueryMemoryContext::Snapshot() const {
    std::scoped_lock lock(mu_);
    return MemoryProfile{
        .used_bytes = used_bytes_,
        .peak_bytes = peak_bytes_,
        .limit_bytes = limit_bytes_,
        .limited = limited_,
    };
}

size_t QueryMemoryContext::limit_bytes() const {
    std::scoped_lock lock(mu_);
    return limit_bytes_;
}

} // namespace pl::flux::execution
