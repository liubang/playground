//=====================================================================
//
// test_async.cpp -
//
// Created by liubang on 2023/05/21 23:30
// Last Modified: 2023/05/21 23:30
//
//=====================================================================

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {
void funcA() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << __FUNCTION__ << std::endl;
}

void funcB() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << __FUNCTION__ << std::endl;
}
} // namespace

auto main(int argc, char *argv[]) -> int {
    std::future<void> f1 = std::async(funcA);
    std::future<void> f2 = std::async(funcB);

    auto start = std::chrono::steady_clock::now();

    f1.get();
    f2.get();

    std::cout << "std::async 耗时："
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << "ms" << std::endl;

    start = std::chrono::steady_clock::now();
    std::thread t1(funcA);
    std::thread t2(funcB);

    t1.join();
    t2.join();

    std::cout << "std::thread 耗时："
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << "ms" << std::endl;

    return 0;
}
