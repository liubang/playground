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

#include <tuple>
#define _XOPEN_SOURCE 700
#include <cstdio>
#include <cstdlib>
#include <ucontext.h>

#define MAX_COROUTINES 128
#define STACK_SIZE     1024 * 64

using coroutine_func = void (*)(void*);

struct Coroutine {
    ucontext_t context;
    coroutine_func func;
    void* arg;
    int active;
    char stack[STACK_SIZE];
};

Coroutine coroutines[MAX_COROUTINES];
int current_coroutine = -1;
ucontext_t main_context;

void coroutine_entry(int id) {
    coroutines[id].func(coroutines[id].arg);
    coroutines[id].active = 0;
    current_coroutine = -1;
    setcontext(&main_context);
}

int coroutine_create(coroutine_func func, void* arg) {
    for (int i = 0; i < MAX_COROUTINES; ++i) {
        if (coroutines[i].active == 0) {
            coroutines[i].func = func;
            coroutines[i].arg = arg;
            coroutines[i].active = 1;

            getcontext(&coroutines[i].context);
            coroutines[i].context.uc_stack.ss_sp = coroutines[i].stack;
            coroutines[i].context.uc_stack.ss_size = STACK_SIZE;
            coroutines[i].context.uc_link = &main_context;
            makecontext(&coroutines[i].context, (void (*)())coroutine_entry, 1, i);
            return i;
        }
    }

    return -1;
}

void coroutine_yield() {
    int id = current_coroutine;
    if (swapcontext(&coroutines[id].context, &main_context) == -1) {
        perror("swapcontext");
        exit(1);
    }
}

void coroutine_resume(int id) {
    if (id < 0 || id >= MAX_COROUTINES || (coroutines[id].active == 0)) {
        return;
    }

    current_coroutine = id;
    if (swapcontext(&main_context, &coroutines[id].context) == -1) {
        perror("swapcontext");
        exit(1);
    }
}

void coro1(void* arg) {
    std::ignore = arg;
    for (int i = 0; i < 10; ++i) {
        printf("Coroutine 1: Step %d\n", i);
        coroutine_yield();
    }
}

void coro2(void* arg) {
    std::ignore = arg;
    for (int i = 0; i < 10; ++i) {
        printf("Coroutine 2: Step %d\n", i);
        coroutine_yield();
    }
}

int main() {
    int id1 = coroutine_create(coro1, nullptr);
    int id2 = coroutine_create(coro2, nullptr);

    while ((coroutines[id1].active != 0) || (coroutines[id2].active != 0)) {
        if (coroutines[id1].active != 0) {
            coroutine_resume(id1);
        }
        if (coroutines[id2].active != 0) {
            coroutine_resume(id2);
        }
    }

    return 0;
}
