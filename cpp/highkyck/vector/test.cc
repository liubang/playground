#include <iostream>
#include <string>
#include "vector.h"

int main(int argc, char* argv[])
{
  ::highkyck::vector<::std::string> vec;
  vec.push_back("hello");
  vec.emplace_back();
  vec.emplace_back("hello", 4);
  for (const auto& str : vec) { ::std::cout << str << '\n'; }
  return 0;
}
