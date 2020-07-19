#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {
  // std::make_unique 是从c++14开始引入的
  std::unique_ptr<std::string> str =
      std::make_unique<std::string>("hello world 1");
  std::cout << str->data() << std::endl;
  std::unique_ptr<std::string> str2(new std::string("hello world 2"));
  std::cout << str2->data() << std::endl;

  std::unique_ptr<std::string> str3 = std::move(str2);
  std::cout << str3->data() << std::endl;

  // core dump
  // std::cout << str2->data() << std::endl;

  // call to deleted constructor
  // std::unique_ptr<std::string> str4 = str3;
  return 0;
}

