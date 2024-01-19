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

#include <list>
#include <memory_resource>
#include <vector>

#include "cpp/tools/measure.h"

std::pmr::synchronized_pool_resource spr;

int main(int argc, char* argv[]) {
    pl::measure("vector1", []() {
        std::pmr::monotonic_buffer_resource mem;
        std::vector<char, std::pmr::polymorphic_allocator<char>> a{
            std::pmr::polymorphic_allocator<char>{&mem}};
        for (int i = 0; i < 65536; ++i) {
            a.push_back(42);
        }
    });

    pl::measure("list1", []() {
        std::pmr::monotonic_buffer_resource mem;
        std::list<char, std::pmr::polymorphic_allocator<char>> a{
            std::pmr::polymorphic_allocator<char>{&mem}};
        for (int i = 0; i < 65536; ++i) {
            a.push_back(42);
        }
    });

    pl::measure("vector2", []() {
        std::vector<char, std::pmr::polymorphic_allocator<char>> a{
            std::pmr::polymorphic_allocator<char>{&spr}};
        for (int i = 0; i < 65536; ++i) {
            a.push_back(42);
        }
    });

    pl::measure("list2", []() {
        std::pmr::monotonic_buffer_resource mem;
        std::list<char, std::pmr::polymorphic_allocator<char>> a{
            std::pmr::polymorphic_allocator<char>{&spr}};
        for (int i = 0; i < 65536; ++i) {
            a.push_back(42);
        }
    });

    return 0;
}
