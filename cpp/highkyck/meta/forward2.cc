#include <iostream>
#include <type_traits>

namespace highkyck {
namespace meta {
class Widget
{
public:
  Widget() {}

  template<typename T, typename std::enable_if<std::is_same<typename std::decay<T>::type,
                                                            std::string>::value>::type* = nullptr>
  explicit Widget(T&& value)
  {
    std::cout << "perfect forwarding" << std::endl;
  }

  Widget(const Widget& value) { std::cout << "copy constructor" << std::endl; }
};
}   // namespace meta
}   // namespace highkyck

int main(int argc, char* argv[])
{
  highkyck::meta::Widget w1;
  const highkyck::meta::Widget w2;
  highkyck::meta::Widget w3(w1);
  highkyck::meta::Widget w4(w2);

  std::string s = "hello";
  highkyck::meta::Widget w5(std::move(s));

  std::string& ss = s;
  highkyck::meta::Widget w6(ss);
  return 0;
}
