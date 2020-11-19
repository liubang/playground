#include <iostream>
#include <memory>

namespace test_weakptr {
class Demo : public std::enable_shared_from_this<Demo> {
 public:
  Demo() = default;
  ~Demo() {
    std::cout << "destructor" << std::endl;
  }
  std::shared_ptr<Demo> getPtr() {
    return shared_from_this();
  }
};
} // namespace test_weakptr

using namespace test_weakptr;
int main(int argc, char* argv[]) {
  std::shared_ptr<Demo> demo1 = std::make_shared<Demo>();
  std::shared_ptr<Demo> demo2(demo1->getPtr());
  std::cout << demo2.use_count() << std::endl;
  std::weak_ptr<Demo> demo3 = demo1->getPtr();
  std::cout << demo3.use_count() << std::endl;
  if (demo3.lock() != nullptr) {
    std::cout << "OK" << std::endl;
  }
  return 0;
}
