// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "absl/status/status.h"

namespace pl::sstv2::merge {

// Minimal forward cursor contract shared by immutable memory tables and SSTs.
// Returned views remain valid until the next mutating cursor operation.
class ForwardCursor {
public:
    virtual ~ForwardCursor() = default;
    [[nodiscard]] virtual absl::Status seek_to_first() = 0;
    [[nodiscard]] virtual absl::Status seek(std::string_view encoded_key) = 0;
    [[nodiscard]] virtual absl::Status next() = 0;
    [[nodiscard]] virtual bool valid() const = 0;
    [[nodiscard]] virtual std::string_view key() const = 0;
    [[nodiscard]] virtual std::string_view value() const = 0;
};

struct Source {
    std::unique_ptr<ForwardCursor> cursor;
    // Smaller values win deterministic ordering when physical keys are equal.
    uint32_t priority = 0;
};

class MergeIterator final {
public:
    explicit MergeIterator(std::vector<Source> sources);

    [[nodiscard]] absl::Status seek_to_first();
    [[nodiscard]] absl::Status seek(std::string_view encoded_key);
    [[nodiscard]] absl::Status next();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string_view key() const;
    [[nodiscard]] std::string_view value() const;
    [[nodiscard]] uint32_t source_priority() const;
    [[nodiscard]] const absl::Status& status() const noexcept { return status_; }

private:
    [[nodiscard]] absl::Status rebuild_heap();
    [[nodiscard]] bool less(size_t lhs, size_t rhs) const;

    std::vector<Source> sources_;
    std::vector<size_t> heap_;
    absl::Status status_;
};

} // namespace pl::sstv2::merge
