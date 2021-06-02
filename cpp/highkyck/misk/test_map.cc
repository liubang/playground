#include <map>
#include <string>
#include <iostream>

int main(int argc, char* argv[]) {
  std::map<std::string, std::string> map;
  try {
    std::cout << map.at("aaa") << '\n';
  } catch (...) {
    std::cout << "exception" << '\n';
  }
  std::cout << map.size() << '\n';
  std::cout << map["aaa"] << '\n';
  std::cout << map.at("aaa") << std::endl;
  std::cout << map.size() << '\n';
  return 0;
}
