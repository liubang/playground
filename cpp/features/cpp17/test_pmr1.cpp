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

#include "cpp/pl/debug/measure.h"
#include <cstdio>
#include <list>
#include <vector>

static char g_buf[65535 * 30];

struct my_mem_resource {
    size_t m_watermark = 0;
    char* m_buf = g_buf;

    char* do_allocate(size_t n, size_t align) {
        m_watermark = (m_watermark + align - 1) / align * align;
        char* p = m_buf + m_watermark;
        m_watermark += n;
        return p;
    }
};

static my_mem_resource* g_mem_resource = new my_mem_resource;

template <typename T> struct my_allocator {
    using value_type = T;

    my_mem_resource* m_resource{nullptr};

    my_allocator() : m_resource(g_mem_resource) {}
    my_allocator(my_mem_resource* m_resource) : m_resource(m_resource) {}

    template <typename U> constexpr my_allocator(my_allocator<U> const& other) noexcept {}
    constexpr bool operator==(my_allocator const& other) const noexcept { return this == &other; }

    T* allocate(size_t n) {
        char* p = m_resource->do_allocate(n * sizeof(T), alignof(T));
        return (T*)p;
    }

    void deallocate(T* p, size_t n) {}
};

int main(int argc, char* argv[]) {
    pl::measure("vector", []() {
        std::vector<char> a;
        a.reserve(65535);
        for (int i = 0; i < 65535; ++i) {
            a.push_back(42);
        }
    });

    // ===============================================================

    pl::measure("list1", []() {
        std::list<char> a;
        for (int i = 0; i < 65535; ++i) {
            a.push_back(42);
        }
    });

    // ===============================================================

    pl::measure("list2", []() {
        std::list<char, my_allocator<char>> a;
        for (int i = 0; i < 65535; ++i) {
            a.push_back(42);
        }
    });

    return 0;
}
