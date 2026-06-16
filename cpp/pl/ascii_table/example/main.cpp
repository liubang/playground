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
// Created: 2025/07/27 22:22

// This example demonstrates various combinations of the pretty::Table
// pipeline API.
//
//   bazel run //cpp/pl/ascii_table/example:main

#include <iostream>

#include "cpp/pl/ascii_table/pretty.h"

using namespace pl::pretty;

// ---------------------------------------------------------------------------
// 1. Basic table
//
//   +===========================+
//   | Name     | Age | Desc     |
//   +===========================+
//   | zhangsan | 12  | student  |
//   | lisi     | 25  | engineer |
//   | wangwu   | 30  | manager  |
//   +===========================+
// ---------------------------------------------------------------------------
void basic() {
    std::cout << "=== Basic table ===\n";
    // clang-format off
    auto t = header("Name", "Age", "Desc")
           | row("zhangsan", "12", "student")
           | row("lisi",     "25", "engineer")
           | row("wangwu",   "30", "manager");
    // clang-format on
    std::cout << t << '\n';
}

// ---------------------------------------------------------------------------
// 2. With separators
//
//   +===========================+
//   | Name     | Age | Desc     |
//   +===========================+
//   | zhangsan | 12  | student  |
//   +───────────────────────────+
//   | lisi     | 25  | engineer |
//   +---------------------------+
//   | wangwu   | 30  | manager  |
//   +===========================+
// ---------------------------------------------------------------------------
void withSeparators() {
    std::cout << "=== With separators ===\n";
    // clang-format off
    auto t = header("Name", "Age", "Desc")
           | row("zhangsan", "12", "student")
           | sep(U'─')
           | row("lisi",   "25", "engineer")
           | sep('-')
           | row("wangwu", "30", "manager");
    // clang-format on
    std::cout << t << '\n';
}

// ---------------------------------------------------------------------------
// 3. CJK characters
//
//   +=================================================+
//   | 姓名     | 年龄 | 城市 | 备注                   |
//   +=================================================+
//   | 张三     | 25   | 北京 | 这是一条很长的备注信息 |
//   | 李四     | 30   | 上海 | 短备注                 |
//   +━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━+
//   | 王五六七 | 28   | 深圳 | 中等长度备注           |
//   +=================================================+
// ---------------------------------------------------------------------------
void cjk() {
    std::cout << "=== CJK characters ===\n";
    // clang-format off
    auto t = header("姓名", "年龄", "城市", "备注")
           | row("张三",     "25", "北京", "这是一条很长的备注信息")
           | row("李四",     "30", "上海", "短备注")
           | sep(U'━')
           | row("王五六七", "28", "深圳", "中等长度备注");
    // clang-format on
    std::cout << t << '\n';
}

// ---------------------------------------------------------------------------
// 4. Auto-expanding columns
//
//   +======================+
//   | Key    |       |     |
//   +======================+
//   | animal | cat   |     |
//   | fruit  | apple | red |
//   +======================+
// ---------------------------------------------------------------------------
void autoExpand() {
    std::cout << "=== Auto-expanding columns ===\n";
    // clang-format off
    auto t = header("Key")                   // 1 col
           | row("animal", "cat")            // 2 cols
           | row("fruit", "apple", "red");   // 3 cols
    // clang-format on
    std::cout << t << '\n';
}

// ---------------------------------------------------------------------------
// 5. Empty cells
//
//   +=======================+
//   | Col A | Col B | Col C |
//   +=======================+
//   | a1    |       | c1    |
//   |       | b2    |       |
//   | a3    | b3    | c3    |
//   +=======================+
// ---------------------------------------------------------------------------
void emptyCells() {
    std::cout << "=== Empty cells ===\n";
    // clang-format off
    auto t = header("Col A", "Col B", "Col C")
           | row("a1", "", "c1")
           | row("",   "b2", "")
           | row("a3", "b3", "c3");
    // clang-format on
    std::cout << t << '\n';
}

// ---------------------------------------------------------------------------
// 6. Borders off (pipe-friendly)
//
//   Method       Result
//   Render()     writes to stdout
//   String()     returns a string
//   RenderTo(w)  writes to io.Writer
// ---------------------------------------------------------------------------
void bordersOff() {
    std::cout << "=== Borders off ===\n";
    // clang-format off
    auto t = header("Method", "Result")
           | row("Render()",    "writes to stdout")
           | row("String()",    "returns a string")
           | sep('-')
           | row("RenderTo(w)", "writes to io.Writer");
    // clang-format on
    t.borders(false);
    std::cout << t << '\n';
}

// ---------------------------------------------------------------------------
// 7. Single column
//
//   +========+
//   | Fruits |
//   +========+
//   | apple  |
//   | banana |
//   | cherry |
//   +========+
// ---------------------------------------------------------------------------
void singleColumn() {
    std::cout << "=== Single column ===\n";
    // clang-format off
    auto t = header("Fruits")
           | row("apple")
           | row("banana")
           | row("cherry");
    // clang-format on
    std::cout << t << '\n';
}

int main() {
    basic();
    withSeparators();
    cjk();
    autoExpand();
    emptyCells();
    bordersOff();
    singleColumn();
    return 0;
}
