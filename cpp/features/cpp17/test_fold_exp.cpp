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

#include <cstdio>
#include <iostream>

template <typename... Ts> auto func(Ts... ts) {
    // print number of parameters
    std::cout << sizeof...(ts) << std::endl;
    // print each parameter
    (printf("%d\n", ts), ...);
    return (0 + ... + ts);
}

int main(int argc, char* argv[]) {
    std::cout << func() << "\n";
    std::cout << func(1, 2) << "\n";
    std::cout << func(1, 2, 3) << "\n";

    return 0;
}
