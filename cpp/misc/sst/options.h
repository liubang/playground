//=====================================================================
//
// options.h -
//
// Created by liubang on 2023/06/01 11:09
// Last Modified: 2023/06/01 11:09
//
//=====================================================================
#pragma once

#include <cstdint>
#include "cpp/misc/sst/comparator.h"
#include "cpp/misc/sst/filter_policy.h"

namespace playground::cpp::misc::sst {

enum class CompressionType : uint8_t {
  kNoCompression = 0x0,
  kSnappyCompression = 0x1,
  kZstdCompression = 0x2,
};

struct Options {
  Options();

  // 4 KB
  std::size_t block_size = 4 * 1024;

  int block_restart_interval = 16;

  uint64_t bits_per_key = 16;

  const Comparator* comparator;

  const FilterPolicy* filter_policy;
};

}  // namespace playground::cpp::misc::sst
