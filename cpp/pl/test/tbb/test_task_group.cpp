//=====================================================================
//
// task_group.cpp -
//
// Created by liubang on 2023/06/17 23:32
// Last Modified: 2023/06/17 23:32
//
//=====================================================================
#include <tbb/task_group.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

void download(const std::string& file) {
    for (int i = 0; i < 10; ++i) {
        std::cout << "Downloading " << file << " (" << i * 10 << "%)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    std::cout << "Download complete: " << file << std::endl;
}

void interact() {
    std::string name;
    std::cin >> name;
    std::cout << "Hi, " << name << std::endl;
}

auto main(int argc, char* argv[]) -> int {
    tbb::task_group tg;
    tg.run([&] {
        download("hello.zip");
    });
    tg.run([&] {
        interact();
    });
    tg.wait();
    return 0;
}
