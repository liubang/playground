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

#ifdef __linux__
#include <ucontext.h>

#include <cstdio>
#include <cstdlib>

#define handle_error(msg)   \
    do {                    \
        perror(msg);        \
        exit(EXIT_FAILURE); \
    } while (00)

namespace {
ucontext_t uctx_main, uctx_func1, uctx_func2;

void func1() {
    if (::swapcontext(&uctx_func1, &uctx_func2) == -1)
        handle_error("swapcontext");
}

void func2() {
    if (::swapcontext(&uctx_func2, &uctx_func1) == -1)
        handle_error("swapcontext");
}
} // namespace

int main(int argc, char* argv[]) {
    char func1_stack[16384];
    char func2_stack[16384];
    if (::getcontext(&uctx_func1) == -1)
        handle_error("getcontext");

    // 设置新的栈
    uctx_func1.uc_stack.ss_sp = func1_stack;
    uctx_func1.uc_stack.ss_size = sizeof(func1_stack);
    // uctx_func1对应的协程执行完执行uctx_main
    uctx_func1.uc_link = &uctx_main;
    // 设置协作的工作函数
    ::makecontext(&uctx_func1, func1, 0);
    if (::getcontext(&uctx_func2) == -1)
        handle_error("getcontext");
    uctx_func2.uc_stack.ss_sp = func2_stack;
    uctx_func2.uc_stack.ss_size = sizeof(func2_stack);
    // uctx_func2执行完执行uctx_func1
    uctx_func2.uc_link = (argc > 1) ? nullptr : &uctx_func1;
    ::makecontext(&uctx_func2, func2, 0);

    // 保存当前执行上下文到uctx_main，然后开始执行uctx_func2对应的上下文
    if (::swapcontext(&uctx_main, &uctx_func2) == -1)
        handle_error("swapcontext");

    ::printf("main: exiting\n");
    return 0;
}
#else
#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << "unsupported system.\n" << std::flush;
    return 0;
}

#endif
