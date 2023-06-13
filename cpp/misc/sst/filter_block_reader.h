//=====================================================================
//
// filter_block_reader.h -
//
// Created by liubang on 2023/05/30 14:50
// Last Modified: 2023/05/30 14:50
//
//=====================================================================

#pragma once

#include "cpp/misc/sst/filter_policy.h"
#include "cpp/tools/binary.h"

namespace pl::misc::sst {

class FilterBlockReader {
 public:
  FilterBlockReader(const FilterPolicy* filter_policy,
                    const pl::tools::Binary& contents);

  bool keyMayMatch(uint64_t block_offset, const tools::Binary& key);

 private:
  const FilterPolicy* filter_policy_;  // filter 策略
  const char* data_;                   //
  const char* offset_;
  size_t num_;
  size_t base_lg_;
};

}  // namespace pl::misc::sst
