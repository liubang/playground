#include "diagnostic.h"

#include <cstdarg>
#include <iostream>

namespace highkyck {
namespace bfcc {

void DiagnosticError(std::string_view source, uint64_t line, uint64_t col,
                     const char* fmt, ...) {
  std::va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "%s\n", source.data());
  std::fprintf(stderr, "%*s^ ", col, "");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
  exit(0);
}

}  // namespace bfcc
}  // namespace highkyck
