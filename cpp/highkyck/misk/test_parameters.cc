#include <iostream>
namespace highkyck {
class Foo {
 public:
  Foo() {
    std::cout << "构造函数" << '\n';
  }
  Foo(const Foo& foo) {
    std::cout << "复制构造" << '\n';
  };
  Foo(const Foo&& foo) {
    std::cout << "移动构造" << '\n';
  }
  Foo& operator=(const Foo& foo) {
    std::cout << "拷贝构造" << '\n';
    return *this;
  }
  Foo& operator=(const Foo&& foo) {
    std::cout << "移动拷贝构造" << '\n';
    return *this;
  }
  ~Foo() = default;
};

class Bar {
 public:
  Bar(const Foo& foo) : foo_(foo) {}

  Foo foo() const {
    return foo_;
  }

 private:
  Foo foo_;
};
} // namespace highkyck

int main(int argc, char* argv[]) {
  highkyck::Foo foo;
  highkyck::Bar bar(foo);

  highkyck::Foo ret = bar.foo();
  // { highkyck::Bar bar(foo); }
  // { highkyck::Bar bar(std::move(foo)); }
  return 0;
}
