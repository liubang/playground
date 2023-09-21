//=====================================================================
//
// test_boost_fiber.cpp -
//
// Created by liubang on 2023/06/05 11:38
// Last Modified: 2023/06/05 11:38
//
//=====================================================================

#include <boost/fiber/all.hpp>
#include <iostream>
#include <string>

void fn(std::string const& str, int n) {
    boost::fibers::fiber::id this_id = boost::this_fiber::get_id();
    for (int i = 0; i < n; ++i) {
        std::cout << "fiber: " << this_id << ", " << str << ": " << std::endl;
        boost::this_fiber::yield();
    }
}

int main(int argc, char* argv[]) {
    boost::fibers::fiber f1(fn, "abc", 3);
    boost::fibers::fiber f2(fn, "def", 4);
    boost::fibers::fiber f3(fn, "hij", 2);
    boost::fibers::fiber f4(fn, "klm", 5);

    f1.join();
    f2.join();
    f3.join();
    f4.join();

    std::cout << "Done." << std::endl;

    return 0;
}
