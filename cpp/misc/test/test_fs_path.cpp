//=====================================================================
//
// test_fs_path.cpp -
//
// Created by liubang on 2023/06/05 23:27
// Last Modified: 2023/06/05 23:27
//
//=====================================================================

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

auto main() -> int {
    fs::path directory = "path/to/directory";
    fs::path filename = "file.txt";

    // Join directory and filename
    fs::path filepath = directory / filename;

    std::cout << "File path: " << filepath << std::endl;

    return 0;
}
