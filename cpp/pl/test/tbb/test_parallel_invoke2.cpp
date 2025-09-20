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

#include <iostream>
#include <string>
#include <tbb/parallel_invoke.h>

int main(int argc, char* argv[]) {
    std::string s = "hello world";
    char ch = 'd';
    tbb::parallel_invoke(
        [&] {
            for (std::size_t i = 0; i < s.size() / 2; ++i) {
                if (s[i] == ch)
                    std::cout << "found!" << std::endl;
            }
        },
        [&] {
            for (std::size_t i = s.size() / 2; i < s.size(); ++i) {
                if (s[i] == ch)
                    std::cout << "found!" << std::endl;
            }
        });

    return 0;
}
