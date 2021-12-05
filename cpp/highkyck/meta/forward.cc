#include <iostream>

namespace highkyck {
namespace meta {
class Widget
{
public:
  Widget() {}

  template<typename T>
  explicit Widget(T&& value)
  {
    std::cout << "perfect forwarding" << std::endl;
  }

  Widget(const Widget& value) { std::cout << "copy constructor" << std::endl; }
};
}  // namespace meta
}  // namespace highkyck

int main(int argc, char* argv[])
{
  highkyck::meta::Widget w1;
  const highkyck::meta::Widget w2;
  highkyck::meta::Widget w3(w1);
  highkyck::meta::Widget w4(w2);


  return 0;
}
