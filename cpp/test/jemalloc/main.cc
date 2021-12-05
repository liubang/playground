#include <cstdio>
#include <memory>
#include <string>

class Foo
{
public:
  Foo(const std::string& name, const std::string& desc)
    : name_(name)
    , desc_(desc)
  {}
  ~Foo() = default;

private:
  std::string name_;
  std::string desc_;
};

int main(int argc, char* argv[])
{
  auto f = std::make_shared<Foo>("haha", "dfdsafa");
  return 0;
}
