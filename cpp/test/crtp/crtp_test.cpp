#include "crtp.h"

// Curiously Recurring Template Pattern (CRTP)
int main(int argc, char* argv[]) {
  test::crtp::Drived1 d;
  d.interface();

  // test::crtp::Drived2 d2;
  return 0;
}
