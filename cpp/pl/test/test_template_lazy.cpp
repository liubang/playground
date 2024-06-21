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

/**
 * 编译器对模板的编译是“惰性”的，即只有当cpp文件中用到了这个模板，该模板里的函数才会被定义；
 * 下面一个模板很显然有语法错误，但是由于没有使用到该模板，所以程序也能正常编译并运行；
 */
// template <typename T> void dummy() {
//     using type = T;
//     // 语法错误!!!
//     "字符串" = 1234;
// }

int main(int argc, char* argv[]) {
    std::cout << "hello world\n";
    return 0;
}
