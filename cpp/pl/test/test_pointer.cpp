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

#include <cassert>
#include <cstdint>
#include <iostream>
#include <type_traits>

namespace {

#pragma pack(push, 1)
struct Foo {
    char a;
    int32_t b;
    int64_t c;
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<Foo> && std::is_trivial_v<Foo>, "Foo is not POD");
static_assert(sizeof(Foo) == 13, "sizeof(Foo) does not equal 13");

#pragma pack(push, 1)
struct Bar {
    char a;
    int32_t b;
    int64_t c;
    char d;
    int32_t e;
    int64_t f;
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<Bar> && std::is_trivial_v<Bar>, "Bar is not POD");
static_assert(sizeof(Bar) == 26, "sizeof(Bar) does not equal 13");

} // namespace

int main(int argc, char* argv[]) {
    Bar b;
    const void* bc = &b;
    const Foo* foo = static_cast<const Foo*>(bc);
    const Foo* fc = foo + 1;

    assert((fc - foo) == 1);

    return 0;
}
