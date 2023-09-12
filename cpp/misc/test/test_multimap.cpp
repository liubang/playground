#include <iostream>
#include <map>

int main(int argc, char* argv[]) {
  std::multimap<std::string, std::string> multimap;

  multimap.insert(std::make_pair("hello", "world"));
  multimap.insert(std::make_pair("hello1", "world1"));
  multimap.insert(std::make_pair("hello", "world2"));
  multimap.insert(std::make_pair("hello", "world3"));

  for (const auto& pair : multimap) {
    std::cout << pair.first << ": " << pair.second << '\n';
  }

  auto range = multimap.equal_range("hello");
  for (auto it = range.first; it != range.second; ++it) {
    std::cout << it->second << '\n';
  }

  return 0;
}
