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

#include <blaze/Blaze.h>
#include <iostream>

int main(int argc, char* argv[]) {
    blaze::StaticMatrix<double, 3UL, 3UL> a{
        {1.0, 2.0, 3.0},
        {4.0, 5.0, 6.0},
        {7.0, 8.0, 9.0},
    };

    blaze::DynamicMatrix<double> b(3UL, 3UL, 2.0);

    // 矩阵加法
    auto c = a + b;

    // 矩阵乘法，a * b的转置矩阵
    auto d = a * trans(b);

    blaze::DynamicVector<double> v{1.0, 2.0, 3.0};

    // 矩阵和向量相乘
    auto w = a * v;

    std::cout << "a + b = \n" << c << std::endl;
    std::cout << "a * trans(b) = \n" << d << std::endl;
    std::cout << "a * v = \n" << w << std::endl;

    return 0;
}
