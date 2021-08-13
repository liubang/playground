#include <iostream>
#include <string>

namespace misc {
class Base
{
public:
  Base()
  {
    std::cout << "Base::constructor" << std::endl;
    print("constructor");
  }
  ~Base()
  {
    std::cout << "Base::destructor" << std::endl;
    print("destructor");
  }

  virtual void print(const std::string& msg) { std::cout << "Base::print::" << msg << std::endl; }
};

class Derive : public Base
{
public:
  Derive()
  {
    std::cout << "Derive::constructor" << std::endl;
    print("constructor");
  }
  ~Derive()
  {
    std::cout << "Derive::destructor" << std::endl;
    print("destructor");
  }

  void print(const std::string& msg) { std::cout << "Derive::print::" << msg << std::endl; }
};
}  // namespace misc

int main(int argc, char* argv[])
{
  misc::Derive d;
  return 0;
}
