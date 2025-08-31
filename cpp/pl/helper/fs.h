// Copyright (c) 2025 The Authors. All rights reserved.
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

#pragma once

#include <fstream>
#include <string>

namespace pl::helpter {

inline std::string file_get_content(std::string const& filename) {
    std::ifstream ifs(filename, std::ios::in | std::ios::binary);
    std::istreambuf_iterator<char> iit(ifs), iite;
    std::string content(iit, iite);
    return content;
}

inline void file_put_content(std::string const& filename, std::string const& content) {
    std::ofstream ofs(filename, std::ios::out | std::ios::binary);
    ofs << content;
}

} // namespace pl::helpter
