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
