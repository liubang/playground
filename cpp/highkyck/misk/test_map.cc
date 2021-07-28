#include <map>
#include <unordered_map>
#include <string>
#include <iostream>

void test1() {
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
}

class Foo {
 public:
  Foo() {
    aaa_.clear();
  }

  std::map<std::string, uint64_t> aaa() {
    return aaa_;
  }

 private:
  std::map<std::string, uint64_t> aaa_;
};

void test2() {
  Foo foo;
  auto a = foo.aaa();
}

int main(int argc, char* argv[]) {
  test1();
  test2();
  return 0;
}
