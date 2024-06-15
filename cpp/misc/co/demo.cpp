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

#if __has_include(<experimental/coroutine>)
#include <experimental/coroutine>

namespace std {
using namespace std::experimental;
}
#elif __has_include(<coroutine>)
#include <coroutine>
#else
#error "No coroutine header"
#endif

#include <iostream>

struct RepeatAwaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> coroutine) const noexcept {
        if (coroutine.done()) {
            return std::noop_coroutine();
        }
        return coroutine;
    }

    void await_resume() const noexcept {}
};

struct RepeatAwaitable {
    RepeatAwaiter operator co_await() { return {}; }
};

struct Promise {
    auto initial_suspend() { return std::suspend_always(); }

    auto final_suspend() noexcept { return std::suspend_always(); }

    void unhandled_exception() { throw; }

    auto yield_value(int ret) {
        mRetValue = ret;
        return RepeatAwaiter();
    }

    void return_void() { mRetValue = 0; }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    int mRetValue;
};

struct Task {
    using promise_type = Promise;

    Task(std::coroutine_handle<promise_type> coroutine) : mCoroutine(coroutine) {}

    std::coroutine_handle<promise_type> mCoroutine;
};

Task hello() {
    std::cout << "hello 1";
    co_yield 2;
    std::cout << "hello 2";
    co_yield 3;
    std::cout << "hello 3";
    co_yield 4;
    std::cout << "hello 4";
    co_return;
}

int main(int argc, char* argv[]) {
    std::cout << "before call hello";
    Task t = hello();
    std::cout << "after call hello";

    while (!t.mCoroutine.done()) {
        t.mCoroutine.resume();
        std::cout << "the return of hello is " << t.mCoroutine.promise().mRetValue;
    }

    return 0;
}
