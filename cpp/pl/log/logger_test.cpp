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

#include "cpp/pl/log/logger.h"

int main(int argc, char* argv[]) {
    int8_t i8 = 1;
    char c = 'c';
    int a = 20;
    long l = 20;
    int32_t i32 = 21;
    uint32_t ui32 = 22;
    float f = 3.14;
    double d = 3.14;
    const char* str = "hello world";
    bool bt = true;
    bool bf = false;

    LOG_TRACE << "char: " << c << ", i8: " << i8 << "int: " << a << ", long: " << l
              << ", i32: " << i32 << ", ui32: " << ui32 << ", float: " << f << ", double: " << d
              << ", str: " << str << ", bool: " << bf << ", bool: " << bt << ", ptr: " << &bt;
    LOG_DEBUG << "char: " << c << ", i8: " << i8 << "int: " << a << ", long: " << l
              << ", i32: " << i32 << ", ui32: " << ui32 << ", float: " << f << ", double: " << d
              << ", str: " << str << ", bool: " << bf << ", bool: " << bt << ", ptr: " << &bt;
    LOG_INFO << "char: " << c << ", i8: " << i8 << "int: " << a << ", long: " << l
             << ", i32: " << i32 << ", ui32: " << ui32 << ", float: " << f << ", double: " << d
             << ", str: " << str << ", bool: " << bf << ", bool: " << bt << ", ptr: " << &bt;
    LOG_WARN << "char: " << c << ", i8: " << i8 << "int: " << a << ", long: " << l
             << ", i32: " << i32 << ", ui32: " << ui32 << ", float: " << f << ", double: " << d
             << ", str: " << str << ", bool: " << bf << ", bool: " << bt << ", ptr: " << &bt;
    LOG_ERROR << "char: " << c << ", i8: " << i8 << "int: " << a << ", long: " << l
              << ", i32: " << i32 << ", ui32: " << ui32 << ", float: " << f << ", double: " << d
              << ", str: " << str << ", bool: " << bf << ", bool: " << bt << ", ptr: " << &bt;
    LOG_FATAL << "char: " << c << ", i8: " << i8 << "int: " << a << ", long: " << l
              << ", i32: " << i32 << ", ui32: " << ui32 << ", float: " << f << ", double: " << d
              << ", str: " << str << ", bool: " << bf << ", bool: " << bt << ", ptr: " << &bt;

    return 0;
}
