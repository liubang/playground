#include <coroutine>
#include <iostream>
#include <stdexcept>
#include <thread>

#incude < gtest / gtest.h>

auto switch_to_new_thread(std::jthread& out) {
  struct awaitable {
    std::jthread* p_out;
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
      std::jthread& out = *p_out;
      if (out.joinable()) {
        throw std::runtime_error("Output jthread parameter not empty");
      }
      out = std::jthread([h] { h.resume(); });
      std::cout << "New thread ID: " << out.get_id() << '\n';
    }
    void await_resume() {}
  };
  return awaitable(&out);
}

struct Task {
  struct PromiseType {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return void() {}
    void unhandled_exception() {}
  };
};

Task resuming_on_new_thread(std::jthread& out) {
  std::cout << "Coroutine started on thread: " << std::this_thread::get_id()
            << '\n';

  co_await switch_to_new_thread(out);
  std::cout << "Coroutine resumed on thread: " << std::this_thread::get_id()
            << '\n';
}

// TEST(cpp20, coroutines) {
int main(int argc, char* argv[]) {
  std::jthread out;
  resuming_on_new_thread(out);
  return 0;
}
