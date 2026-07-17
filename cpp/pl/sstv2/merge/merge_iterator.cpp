// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/sstv2/merge/merge_iterator.h"

#include <algorithm>
#include <new>
#include <tuple>

#include "absl/status/status.h"

namespace pl::sstv2::merge {

MergeIterator::MergeIterator(std::vector<Source> sources) : sources_(std::move(sources)) {
    try {
        heap_.reserve(sources_.size());
    } catch (const std::bad_alloc&) {
        status_ = absl::ResourceExhaustedError("merge heap allocation failed");
        return;
    }
    for (const auto& source : sources_) {
        if (source.cursor == nullptr) {
            status_ = absl::InvalidArgumentError("merge source cursor is null");
            break;
        }
    }
}

bool MergeIterator::less(size_t lhs, size_t rhs) const {
    const auto& left = sources_[lhs];
    const auto& right = sources_[rhs];
    return std::tuple(left.cursor->key(), left.priority, lhs) >
           std::tuple(right.cursor->key(), right.priority, rhs);
}

absl::Status MergeIterator::rebuild_heap() {
    heap_.clear();
    try {
        for (size_t i = 0; i < sources_.size(); ++i) {
            if (sources_[i].cursor->valid()) {
                heap_.push_back(i);
            }
        }
    } catch (const std::bad_alloc&) {
        status_ = absl::ResourceExhaustedError("merge heap allocation failed");
        heap_.clear();
        return status_;
    }
    std::make_heap(
        heap_.begin(), heap_.end(), [this](size_t lhs, size_t rhs) { return less(lhs, rhs); });
    return absl::OkStatus();
}

absl::Status MergeIterator::seek_to_first() {
    if (!status_.ok()) {
        return status_;
    }
    for (auto& source : sources_) {
        status_ = source.cursor->seek_to_first();
        if (!status_.ok()) {
            heap_.clear();
            return status_;
        }
    }
    return rebuild_heap();
}

absl::Status MergeIterator::seek(std::string_view encoded_key) {
    if (!status_.ok()) {
        return status_;
    }
    for (auto& source : sources_) {
        status_ = source.cursor->seek(encoded_key);
        if (!status_.ok()) {
            heap_.clear();
            return status_;
        }
    }
    return rebuild_heap();
}

absl::Status MergeIterator::next() {
    if (!status_.ok()) {
        return status_;
    }
    if (heap_.empty()) {
        return absl::FailedPreconditionError("merge iterator is not valid");
    }
    std::pop_heap(
        heap_.begin(), heap_.end(), [this](size_t lhs, size_t rhs) { return less(lhs, rhs); });
    const size_t current = heap_.back();
    heap_.pop_back();
    status_ = sources_[current].cursor->next();
    if (!status_.ok()) {
        heap_.clear();
        return status_;
    }
    if (sources_[current].cursor->valid()) {
        heap_.push_back(current);
        std::push_heap(
            heap_.begin(), heap_.end(), [this](size_t lhs, size_t rhs) { return less(lhs, rhs); });
    }
    return absl::OkStatus();
}

bool MergeIterator::valid() const noexcept {
    return status_.ok() && !heap_.empty();
}

std::string_view MergeIterator::key() const {
    return valid() ? sources_[heap_.front()].cursor->key() : std::string_view{};
}

std::string_view MergeIterator::value() const {
    return valid() ? sources_[heap_.front()].cursor->value() : std::string_view{};
}

uint32_t MergeIterator::source_priority() const {
    return valid() ? sources_[heap_.front()].priority : 0;
}

} // namespace pl::sstv2::merge
