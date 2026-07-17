// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pl::sstv2::buffer {

class BufferWriter {
public:
    BufferWriter() = default;
    explicit BufferWriter(size_t initial_capacity) { buffer_.reserve(initial_capacity); }

    [[nodiscard]] size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] size_t capacity() const noexcept { return buffer_.capacity(); }
    [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
    [[nodiscard]] std::string_view view() const noexcept { return buffer_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::as_bytes(std::span(buffer_.data(), buffer_.size()));
    }

    void clear() noexcept { buffer_.clear(); }
    void reserve(size_t capacity) { buffer_.reserve(capacity); }

    void append(std::string_view value) { buffer_.append(value); }

    void append(std::span<const std::byte> value) {
        if (value.empty()) {
            return;
        }
        const auto* source = reinterpret_cast<const char*>(value.data());
        const auto source_address = reinterpret_cast<uintptr_t>(source);
        const auto begin_address = reinterpret_cast<uintptr_t>(buffer_.data());
        const auto end_address = begin_address + buffer_.size();
        if (source_address >= begin_address && source_address < end_address) {
            const size_t offset = source_address - begin_address;
            const size_t length = value.size();
            buffer_.append(buffer_, offset, length);
            return;
        }
        buffer_.append(source, value.size());
    }

    [[nodiscard]] std::span<std::byte> append_space(size_t length) {
        const size_t offset = buffer_.size();
        buffer_.resize(offset + length);
        return std::as_writable_bytes(std::span(buffer_.data() + offset, length));
    }

    [[nodiscard]] std::string release() && noexcept { return std::move(buffer_); }

private:
    std::string buffer_;
};

} // namespace pl::sstv2::buffer
