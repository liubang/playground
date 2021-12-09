#include <sys/stat.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  std::string filename = "/tmp/ccFBYHfm.s";
  struct stat result;
  if (stat(filename.c_str(), &result) == 0) {
    time_t mod_time = result.st_mtime;
    // std::cout << mod_time << std::endl;
    // if (mod_time > 1000) {
    std::cout << typeid(result).name() << std::endl;
    std::cout << typeid(mod_time).name() << std::endl;
    // }
  }
  return 0;
}
// 1596187132
