#include <array>
#include <atomic>

namespace highkyck {
class Foo {
 public:
  Foo() {
    for (auto& el : data_) {
      el.store(0, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  uint64_t read(const uint64_t index) { return data_.at(index); }

  void write(const uint64_t index, const uint64_t value) {
    data_.at(index) = value;
  }

 private:
  std::array<std::atomic<uint64_t>, 100> data_;
};
}  // namespace highkyck

int main(int argc, char* argv[]) {
  highkyck::Foo foo;
  foo.write(1, 10);
  foo.read(1);
  return 0;
}
