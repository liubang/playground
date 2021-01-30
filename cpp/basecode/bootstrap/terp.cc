#include "terp.h"

namespace basecode {

Terp::Terp(uint32_t heap_size) : heap_size_(heap_size) {}

Terp::~Terp() {
  delete[] heap_;
  heap_ = nullptr;
}

bool Terp::initialize(Result& result) {
  heap_ = new uint8_t[heap_size_];
  registers_.pc = 0;
  registers_.fr = 0;
  registers_.sr = 0;
  registers_.sp = heap_size_;

  for (size_t i = 0; i < 64; ++i) {
    registers_.i[i] = 0;
    registers_.f[i] = 0.0;
  }

  return !result.is_failed();
}

} // namespace basecode
