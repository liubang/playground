#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
#include <folly/init/Init.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace {
std::shared_ptr<folly::CPUThreadPoolExecutor> thread_pool =
    std::make_shared<folly::CPUThreadPoolExecutor>(
        200,
        std::make_shared<folly::NamedThreadFactory>("LoaderDataPool"));
}

int addSync(int a, int b) {
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  return a + b;
}

folly::Future<int> add(int a, int b) {
  auto promise = std::make_shared<folly::Promise<int>>();
  thread_pool->add([a, b, promise]() { promise->setValue(addSync(a, b)); });
  return promise->getFuture();
}

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv);
  std::cout << "======test1======" << std::endl;
  add(1, 2).then(
      [](folly::Try<int>&& value) { std::cout << value.value() << std::endl; });

  std::cout << "======test2======" << std::endl;

  std::cout << "batch async:" << std::endl;
  std::vector<folly::Future<int>> responses;
  for (int i = 0; i < 100; i++) {
    auto ret = add(1, i);
    responses.emplace_back(std::move(ret));
  }

  std::cout << "sleep 2000 ms:" << std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  std::cout << "get async result:" << std::endl;

  folly::collectAll(responses)
      .via(folly::getCPUExecutor().get())
      .then([](folly::Try<std::vector<folly::Try<int>>> try_responses) {
        for (folly::Try<int>& try_response : try_responses.value()) {
          std::cout << try_response.value() << std::endl;
        }
      })
      .get();

  return 0;
}
