#include "diagnostic.h"

#include <cstdarg>
#include <iostream>

namespace highkyck {
namespace bfcc {

void DiagnosticError(std::string_view source, int64_t line, int64_t col,
                     const char* fmt, ...) {
  std::va_list ap;
  va_start(ap, fmt);
  std::cerr << source << std::endl;
  std::fprintf(stderr, "%*s^ ", col, "");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
  exit(0);
}

}  // namespace bfcc
}  // namespace highkyck
