#include <iostream>

#include "ip.h"

int main(int argc, char* argv[]) {
  std::cout << highkyck::utils::getLocalIp().value() << std::endl;
  return 0;
}
