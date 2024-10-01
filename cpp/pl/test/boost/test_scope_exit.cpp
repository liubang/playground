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

#include <boost/scope_exit.hpp>
#include <iostream>

class Foo {
public:
    Foo() { std::cout << __FUNCTION__ << '\n'; }
    ~Foo() { std::cout << __FUNCTION__ << '\n'; }
};

int main(int argc, char* argv[]) {
    std::cout << "start\n";
    Foo foo;

    int a = 10;

    BOOST_SCOPE_EXIT(&a) { std::cout << "scope exit, a=" << a << '\n'; }
    BOOST_SCOPE_EXIT_END;

    a++;

    std::cout << "bye~, a=" << a << '\n';

    return 0;
}