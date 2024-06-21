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
#include <cstdlib>

static int operands[64 * 1024], cells[1024 * 1024], i, pc, l;

// https://zh.wikipedia.org/wiki/Brainfuck

int main(int c, char* v[]) {
    if (c < 2)
        exit(1);
    for (FILE* f = fopen(v[1], "r"); (f != nullptr) && (c = getc(f)) != EOF; operands[i++] = c)
        ;
    for (i = 0; (c = operands[i]) != 0; i++) {
        // 指针加一
        if (c == '>')
            pc++;
        // 指针减一
        if (c == '<')
            pc--;
        // 指针指向的字节的值加一
        if (c == '+')
            cells[pc]++;
        // 指针指向的字节的值减一
        if (c == '-')
            cells[pc]--;
        // 输出指针指向的单元内容（ASCII码）
        if (c == '.')
            putc(cells[pc], stdout);
        // 输入内容到指针指向的单元（ASCII码）
        if (c == ',')
            cells[pc] = getchar();
        // 如果指针指向的单元值为零，向后跳转到对应的]指令的次一指令处
        for (l = 0; c == '[' && (cells[pc] == 0); i++) {
            if (operands[i] == '[')
                l++;
            if ((operands[i] == ']') && (l-- == 1))
                break;
        }
        // 如果指针指向的单元值不为零，向前跳转到对应的[指令的次一指令处
        for (; c == ']' && (cells[pc] != 0); i--) {
            if (operands[i] == ']')
                l++;
            if ((operands[i] == '[') && (l-- == 1))
                break;
        }
    }
}
