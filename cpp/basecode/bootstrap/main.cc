#include <iostream>

#include "terp.h"

int main(int argc, char* argv[]) {
  basecode::Terp terp(1024 * 1024 * 32);
  basecode::Result result;

  if (!terp.initialize(result)) {
    std::cerr << "oh, fuck...";
  }

  return 0;
}
