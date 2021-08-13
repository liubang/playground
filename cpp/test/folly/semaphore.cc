#include <chrono>
#include <folly/synchronization/SaturatingSemaphore.h>

namespace test {
class Foo
{
public:
  void run(int millseconds)
  {
    semaphore_.try_wait_until(std::chrono::steady_clock::now() +
                              std::chrono::microseconds(millseconds));
  }

private:
  folly::SaturatingSemaphore<true> semaphore_;
};
}  // namespace test

int main(int argc, char* argv[])
{
  ::test::Foo foo;
  for (int i = 0; i < 1000000; ++i) { foo.run(1000); }
  return 0;
}
