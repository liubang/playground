#include "terp.h"

namespace basecode {

Terp::Terp(uint32_t heap_size) : heap_size_(heap_size) {}

Terp::~Terp() {
  delete[] heap_;
  heap_ = nullptr;
}

bool Terp::initialize(Result& result) {
  auto heap_size_in_qwords = heap_size_ / sizeof(uint64_t);
  heap_ = new uint64_t[heap_size_in_qwords];
  registers_.pc = 0;
  registers_.fr = 0;
  registers_.sr = 0;
  registers_.sp = heap_size_in_qwords;

  for (size_t i = 0; i < 64; ++i) {
    registers_.i[i] = 0;
    registers_.f[i] = 0.0;
  }

  return !result.is_failed();
}

uint64_t Terp::pop() {
  uint64_t value = *(static_cast<uint64_t*>(&heap_[registers_.sp]));
  registers_.sp += sizeof(uint64_t);
  return value;
}

void Terp::push(uint64_t value) {
  registers_.sp -= sizeof(uint64_t);
  heap_[registers_.sp] = value;
}

const RegisterFileT& Terp::register_file() const {
  return registers_;
}

} // namespace basecode
