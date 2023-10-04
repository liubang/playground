//=====================================================================
//
// test_pmr2.cpp -
//
// Created by liubang on 2023/10/05 01:15
// Last Modified: 2023/10/05 01:15
//
//=====================================================================

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
