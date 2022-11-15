#include <iostream>
#include <memory>
#include <string>

namespace highkyck {

class Foo {
 public:
  Foo(const std::shared_ptr<std::string>& name) : name_ptr_(name) {}

  virtual ~Foo() = default;

  const std::shared_ptr<std::string>& name_ptr() const { return name_ptr_; }

  void print_count() const {
    std::cout << "Foo::name: " << name_ptr_.use_count() << "\n";
  }

 private:
  std::shared_ptr<std::string> name_ptr_;
};

}  // namespace highkyck

int main(int argc, char* argv[]) {
  std::shared_ptr<std::string> name = std::make_shared<std::string>("name");

  std::cout << "name: " << name.use_count() << "\n";
  highkyck::Foo foo = highkyck::Foo(name);
  std::cout << "name: " << name.use_count() << "\n";

  foo.print_count();

  auto ret = foo.name_ptr();

  foo.print_count();

  std::cout << "ret: " << ret.use_count() << std::endl;
  return 0;
}
