#include <coroutine>
#include <exception>
#include <iostream>

template <typename T>
struct Generator {
  struct promise_type {
    T value;
    std::suspend_always initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(T t) {
      value = t;
      return {};
    }
    void unhandled_exception() {}
    Generator<T> get_return_object() {
      return Generator<T>{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    void return_void() {}
  };

  std::coroutine_handle<promise_type> handle;

  T next() {
    handle.resume();
    return handle.promise().value;
  }
};

template <typename T>
Generator<T> sequence() {
  int i = 0;
  while (true) {
    co_yield i++;
  }
  co_return;
}

int main(int argc, char* argv[]) {
  auto gen = sequence<int64_t>();
  for (int i = 0; i < 10; ++i) {
    std::cout << gen.next() << std::endl;
  }
  return 0;
}
