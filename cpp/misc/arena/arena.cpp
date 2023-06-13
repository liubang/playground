//=====================================================================
//
// arena.cpp -
//
// Created by liubang on 2023/05/21 01:37
// Last Modified: 2023/05/21 01:37
//
//=====================================================================
#include "cpp/misc/arena/arena.h"

#include <cassert>

namespace pl::misc::arena {

constexpr int BLOCK_SIZE = 4096;
constexpr int POINTER_SIZE = 8;

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() {
  for (auto& block : blocks_) {
    delete[] block;
  }
}

char* Arena::allocate_aligned(std::size_t bytes) {
  constexpr int align =
      (sizeof(void*) > POINTER_SIZE) ? sizeof(void*) : POINTER_SIZE;
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be power of 2");

  std::size_t current_mod =
      reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  std::size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  std::size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    result = allocate_fallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

char* Arena::allocate_fallback(std::size_t bytes) {
  // 如果申请的内存大于一个Block的1/4，则需要多少内存就申请多少内存
  // 否则，申请的内存太小的话，就按一个block来申请
  if (bytes > BLOCK_SIZE / 4) {
    char* result = allocate_new_block(bytes);
    return result;
  }

  alloc_ptr_ = allocate_new_block(BLOCK_SIZE);
  alloc_bytes_remaining_ = BLOCK_SIZE;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::allocate_new_block(std::size_t bytes) {
  char* result = new char[bytes];
  blocks_.push_back(result);
  // block_bytes + pointer
  memory_usage_.fetch_add(bytes + sizeof(char*), std::memory_order_relaxed);
  return result;
}

}  // namespace pl::misc::arena
