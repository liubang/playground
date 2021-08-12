#include <string>
#include <iostream>
#include <vector>
#include <cstring>
#include "hash.h"

namespace {
std::vector<std::string> strs = {"OatoXR1oWQ27aghr",
                                 "FsuHfYSASnISaLXK",
                                 "0u56dghh6A2VpoqW",
                                 "$Q$0pjppVzm367Vr",
                                 "5wA#jO9nuLEUBKWf",
                                 "lfb8M5E4ZqqgFLUJ",
                                 "gF#580VAxlU8SYjl",
                                 "helloworld",
                                 "$poskBwgBjiR2%dFL",
                                 "N#zvGO2EDbOKast9gY",
                                 "lJe1H3X6BM5kB!#BFR8"};
}

int main(int argc, char* argv[])
{
  std::cout << sizeof(size_t) << std::endl;
  std::cout << static_cast<size_t>(10 * 0.69) << std::endl;
  for (auto& str : strs) {
    std::cout << str << ": " << test::Hash(str.data(), str.size(), 0xbc9f1d34) << std::endl;
  }

  const char* str = "hello";
  std::cout << ::strlen(str) << std::endl;
  return 0;
}
