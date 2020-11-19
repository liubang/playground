#include <string>
#include <iostream>
#include <algorithm>

int main(int argc, char* argv[]) {
  std::string str = " dfjaklsf  djlkasf , : dasfsad   ";
  auto it = std::remove_if(str.begin(), str.end(), ::isspace);
  str.erase(it, str.end());
  std::cout << str << std::endl;
  return 0;
}
