#pragma once

#include <string_view>

namespace highkyck {
namespace bfcc {

[[noreturn]] void DiagnosticError(std::string_view source, int64_t line,
                                  int64_t col, const char* fmt, ...);

}  // namespace bfcc
}  // namespace highkyck
