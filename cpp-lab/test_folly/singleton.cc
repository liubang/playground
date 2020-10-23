#include <folly/Singleton.h>
#include <folly/init/Init.h>
#include <string>
#include <iostream>
#include "singleton.h"

namespace {
struct PrivateTag {};
} // namespace

namespace foo {
static folly::Singleton<Foo, PrivateTag> foo_instance([]() {
  return new Foo("liubang", 25);
});

std::shared_ptr<Foo> Foo::getInstance() {
  return foo_instance.try_get();
}

} // namespace foo

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv);

  auto f1 = foo::Foo::getInstance();
  auto f2 = foo::Foo::getInstance();
  std::cout << f1->getName() << "," << f1->getAge() << std::endl;
  std::cout << f2->getName() << "," << f2->getAge() << std::endl;
  std::cout << &(*f1) << std::endl;
  std::cout << &(*f2) << std::endl;
  return 0;
}
