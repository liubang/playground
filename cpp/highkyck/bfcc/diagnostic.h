#pragma once

#include <string_view>

namespace highkyck {
namespace bfcc {

[[noreturn]] void DiagnosticError(std::string_view source, uint64_t line,
                                  uint64_t col, const char* fmt, ...);

}  // namespace bfcc
}  // namespace highkyck
