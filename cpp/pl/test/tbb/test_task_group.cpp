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

#include <chrono>
#include <iostream>
#include <string>
#include <tbb/task_group.h>
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
