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

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace pl {

namespace detail {

template <std::size_t SIZE> class FixedBuffer {
public:
    FixedBuffer(const FixedBuffer&) = delete;
    FixedBuffer(FixedBuffer&&) = delete;
    FixedBuffer& operator=(const FixedBuffer&) = delete;
    FixedBuffer& operator=(FixedBuffer&&) = delete;

    FixedBuffer() : cursor_(data_) {}
    ~FixedBuffer() = default;

    void append(std::string_view buf) {
        if (avail() > buf.size()) {
            std::memcpy(cursor_, buf.data(), buf.size());
            cursor_ += buf.size();
        }
    }

    [[nodiscard]] const char* data() const { return data_; }
    [[nodiscard]] const char* current() const { return cursor_; }
    [[nodiscard]] std::size_t size() const { return cursor_ - data_; }
    [[nodiscard]] std::size_t avail() const { return end() - cursor_; }
    [[nodiscard]] bool full() const { return avail() == 0; }

    void reset() { cursor_ = data_; }

private:
    [[nodiscard]] const char* end() const { return data_ + sizeof(data_); }

private:
    char data_[SIZE];
    char* cursor_;
};

} // namespace detail

class LogStream {
    using Buffer = detail::FixedBuffer<4000>;

public:
    LogStream() = default;
    // noncopyable and nonmoveable
    LogStream(const LogStream&) = delete;
    LogStream(LogStream&&) = delete;
    LogStream& operator=(const LogStream&) = delete;
    LogStream& operator=(LogStream&&) = delete;

    LogStream& operator<<(const char* const v) {
        write(std::string_view(v));
        return *this;
    }

    LogStream& operator<<(const std::string& v) {
        write(std::string_view(v));
        return *this;
    }

    LogStream& operator<<(std::string_view v) {
        write(v);
        return *this;
    }

    template <typename T,
              std::enable_if_t<std::disjunction_v<std::is_same<T, char>,
                                                  std::is_same<T, signed char>,
                                                  std::is_same<T, unsigned char>,
                                                  std::is_arithmetic<T>,
                                                  std::is_floating_point<T>>>* = nullptr>
    LogStream& operator<<(const T v) {
        write<T>(v);
        return *this;
    }

    LogStream& operator<<(const bool v) {
        if (v) {
            write("true");
        } else {
            write("false");
        }
        return *this;
    }

    const Buffer& buffer() const { return buffer_; }

private:
    template <typename T,
              std::enable_if_t<std::disjunction_v<std::is_same<T, char>,
                                                  std::is_same<T, unsigned char>,
                                                  std::is_same<T, int8_t>,
                                                  std::is_same<T, uint8_t>>>* = nullptr>
    void write(const T data) {
        write(std::string_view(&data, sizeof(data)));
    }

    template <typename T, std::enable_if_t<std::is_arithmetic<T>::value>* = nullptr>
    void write(const T data) {
        if (buffer_.full()) {
            return;
        }
        // TODO(liubang): 使用更加高效的转换
        auto s = std::to_string(data);
        write(std::string_view(s));
    }

    void write(std::string_view data);

private:
    Buffer buffer_;
};

} // namespace pl
