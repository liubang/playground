#include "variable_template.h"

int main(int argc, char* argv[]) {
  static_assert(!test::cpp14::is_digit<'x'>, "ok");
  static_assert(test::cpp14::is_digit<'0'>, "ok");
  static_assert(test::cpp14::is_digit<'9'>, "ok");
  static_assert(test::cpp14::is_digit<'5'>, "ok");
  return 0;
}
