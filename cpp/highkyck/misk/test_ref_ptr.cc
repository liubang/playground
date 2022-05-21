#include <iostream>

#include "includes/ref_ptr.h"

namespace highkyck {
class Foo final : public Referenceable
{
};

using FooPtr = RefPtr<Foo>;

class Bar
{
public:
  Bar(const FooPtr &foo_ptr) : foo_ptr_(foo_ptr) {}

  void print_ref_count() { std::cout << "Bar::foo_ptr: " << foo_ptr_->GetRef() << "\n"; }

  FooPtr &foo_ptr_ref() { return foo_ptr_; }

  FooPtr foo_ptr() { return foo_ptr_; }

private:
  FooPtr foo_ptr_;
};

}// namespace highkyck

int main(int argc, char *argv[])
{
  highkyck::FooPtr foo_ptr = new highkyck::Foo;
  std::cout << "foo_ptr: " << foo_ptr->GetRef() << "\n";
  highkyck::Bar bar(foo_ptr);
  std::cout << "foo_ptr: " << foo_ptr->GetRef() << "\n";
  bar.print_ref_count();

  std::cout << "++++++++++++++++++++\n";
  {
    highkyck::FooPtr &ret = bar.foo_ptr_ref();
    bar.print_ref_count();
    std::cout << "foo_ptr: " << foo_ptr->GetRef() << "\n";
    std::cout << "ret: " << ret->GetRef() << "\n";
  }

  std::cout << "++++++++++++++++++++\n";
  {
    highkyck::FooPtr ret = bar.foo_ptr();
    bar.print_ref_count();
    std::cout << "foo_ptr: " << foo_ptr->GetRef() << "\n";
    std::cout << "ret: " << ret->GetRef() << "\n";
  }

  std::cout << "++++++++++++++++++++\n";
  bar.print_ref_count();
  std::cout << "foo_ptr: " << foo_ptr->GetRef() << "\n";

  return 0;
}
