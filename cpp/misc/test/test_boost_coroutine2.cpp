//=====================================================================
//
// test_boost_coroutine2.cpp -
//
// Created by liubang on 2023/06/05 10:04
// Last Modified: 2023/06/05 10:04
//
//=====================================================================
#include <boost/coroutine2/all.hpp>
#include <iostream>

int main(int argc, char* argv[]) {
    typedef boost::coroutines2::coroutine<int> coro_t;

    coro_t::pull_type source([&](coro_t::push_type& sink) {
        int first = 1, second = 1;
        sink(first);
        sink(second);
        for (int i = 0; i < 8; ++i) {
            int third = first + second;
            first = second;
            second = third;
            sink(third);
        }
    });

    for (auto i : source)
        std::cout << i << " ";

    return 0;
}
