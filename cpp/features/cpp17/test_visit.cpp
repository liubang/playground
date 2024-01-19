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

#include <variant>
#include <vector>

std::variant<std::false_type, std::true_type> bool_variant(bool x) {
    if (x) {
        return std::true_type{};
    }
    return std::false_type{};
}

void saxpy(std::vector<float> a, float b, float c) {
    auto has_b = bool_variant(b != 1);
    auto has_c = bool_variant(b != 0);
    std::visit(
        [&](auto has_b, auto has_c) {
            for (auto& ai : a) {
                if constexpr (has_b) {
                    ai *= b;
                }
                if constexpr (has_c) {
                    ai += c;
                }
            }
        },
        has_b, has_c);
}

int main(int argc, char* argv[]) {
    // auto func = [&](auto val) {
    //     static int _ = printf("instanced!!!\n");
    //     std::cout << val << std::endl;
    // };
    //
    // func(std::integral_constant<int, 1>{});
    // func(std::integral_constant<int, 2>{});
    // func(std::integral_constant<int, 3>{});

    // std::variant<std::false_type, std::true_type> v;
    // v = std::false_type{};
    // v = std::true_type{};
    //
    // std::visit(
    //     [&](auto val) {
    //         static int _ = printf("instanced!!!!\n");
    //         if constexpr (val) {
    //             std::cout << "true" << std::endl;
    //         } else {
    //             std::cout << "false" << std::endl;
    //         }
    //     },
    //     v);
    //

    std::vector<float> a(1024);
    saxpy(a, 1, 0);
    saxpy(a, 1, 3.14);
    saxpy(a, 2, 0);
    saxpy(a, 2, 3.14);

    return 0;
}
