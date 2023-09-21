//=====================================================================
//
// parallel_invoke.cpp -
//
// Created by liubang on 2023/06/18 00:07
// Last Modified: 2023/06/18 00:07
//
//=====================================================================
#include <tbb/parallel_invoke.h>

#include <iostream>
#include <string>

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

int main(int argc, char* argv[]) {
    tbb::parallel_invoke(
        [&] {
            download("hello.zip");
        },
        [&] {
            interact();
        });
    return 0;
}
