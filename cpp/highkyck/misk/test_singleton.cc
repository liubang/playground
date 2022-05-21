#include <iostream>

#include "includes/singleton.h"

namespace highkyck {
class Foo
{
public:
  Foo() = default;
  virtual ~Foo() = default;
};
}// namespace highkyck

int main(int argc, char *argv[])
{
  highkyck::misk::Singleton<highkyck::Foo> foo;
  highkyck::Foo *f1 = foo.getInstance();
  highkyck::Foo *f2 = foo.getInstance();

  std::cout << f1 << std::endl;
  std::cout << f2 << std::endl;
  return 0;
}
