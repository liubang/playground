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

#include <csetjmp>
#include <functional>
#include <iostream>
#include <vector>

struct Coroutine;

enum class CoroutineState : uint8_t {
    RUNNING,
    STOP,
    SUSPEND,
    DIE,
};

// clang-format off
#define CASE(x) case x: return #x
// clang-format on
const char* StateToString(CoroutineState state) {
    switch (state) {
        CASE(CoroutineState::RUNNING);
        CASE(CoroutineState::STOP);
        CASE(CoroutineState::SUSPEND);
        CASE(CoroutineState::DIE);
    }
}
#undef CASE

struct Coroutine {
    CoroutineState state = CoroutineState::STOP;
    Coroutine* from = nullptr;
    std::jmp_buf buf;

    std::function<void(void)> func;
    const char* name;

    Coroutine(std::function<void(void)>&& func, const char* name)
        : func(std::move(func)), name(name) {}
    ~Coroutine() { std::cout << name << " destroyed" << std::endl; }
};

struct {
    std::jmp_buf start_buf;
    Coroutine* current_co = nullptr;
} Context;

#define co_return()                                              \
    {                                                            \
        Context.current_co->state = CoroutineState::DIE;         \
        Context.current_co = Context.current_co->from;           \
        if (Context.current_co != nullptr) {                     \
            Context.current_co->state = CoroutineState::RUNNING; \
            std::longjmp(Context.current_co->buf, 2);            \
        } else {                                                 \
            std::longjmp(Context.start_buf, 2);                  \
        }                                                        \
    }

#define co_resume(co)                                                    \
    do {                                                                 \
        switch (co->state) {                                             \
        case CoroutineState::STOP:                                       \
            if (!Context.current_co) {                                   \
                if (setjmp(Context.start_buf) == 0) {                    \
                    co->state = CoroutineState::RUNNING;                 \
                    Context.current_co = co;                             \
                    co->func();                                          \
                }                                                        \
            } else {                                                     \
                if (setjmp(Context.current_co->buf) == 0) {              \
                    Context.current_co->state = CoroutineState::SUSPEND; \
                    co->from = Context.current_co;                       \
                    co->state = CoroutineState::RUNNING;                 \
                    Context.current_co = co;                             \
                    co->func();                                          \
                }                                                        \
            }                                                            \
            break;                                                       \
        case CoroutineState::SUSPEND:                                    \
            if (!Context.current_co) {                                   \
                Context.current_co = co;                                 \
            }                                                            \
            if (Context.current_co->from) {                              \
                std::longjmp(Context.current_co->from->buf, 2);          \
            } else {                                                     \
                if (setjmp(Context.start_buf) == 0) {                    \
                    std::longjmp(Context.current_co->buf, 2);            \
                }                                                        \
            }                                                            \
            break;                                                       \
        default:;                                                        \
        }                                                                \
    } while (0);

#define co_yield()                                               \
    do {                                                         \
        if (setjmp(Context.current_co->buf) == 0) {              \
            Context.current_co->state = CoroutineState::SUSPEND; \
            if (Context.current_co->from) {                      \
                Context.current_co = Context.current_co->from;   \
                std::longjmp(Context.current_co->buf, 2);        \
            } else {                                             \
                Context.current_co = nullptr;                    \
                std::longjmp(Context.start_buf, 2);              \
            }                                                    \
        }                                                        \
    } while (0)

namespace details {
std::vector<Coroutine*> cos;
}

void ReadNextResourceName() {
    std::cout << "read source name OK" << std::endl;
    co_return ();
}

void DownloadSource() {
    std::cout << "downloading" << std::endl;
    // start another coroutine
    auto* co = new Coroutine(ReadNextResourceName, "ReadNextResourceName");
    details::cos.push_back(co);

    co_resume(co);
    std::cout << "coutinue download" << std::endl;

    co_yield ();
    std::cout << "after yield" << std::endl;

    co_return ();
}

int main(int argc, char* argv[]) {
    auto* co = new Coroutine(DownloadSource, "DownloadSource");
    details::cos.push_back(co);
    std::cout << co->name << "'s state = " << StateToString(co->state) << std::endl;
    co_resume(co);
    std::cout << co->name << "'s state = " << StateToString(co->state) << std::endl;
    co_resume(co);
    std::cout << "at end of main" << std::endl;

    for (auto* c : details::cos) {
        delete c;
    }
    details::cos.clear();
    return 0;
}
