//=====================================================================
//
// filter_block_builder.h -
//
// Created by liubang on 2023/05/29 19:48
// Last Modified: 2023/05/29 19:48
//
//=====================================================================

#pragma once

#include <string>
#include <vector>

#include "cpp/misc/sst/filter_policy.h"
#include "cpp/tools/binary.h"

namespace playground::cpp::misc::sst {

class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy* filter_policy);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterPolicy& operator=(const FilterBlockBuilder&) = delete;

  void startBlock(uint64_t offset);

  void addKey(const tools::Binary& key);

  tools::Binary finish();

 private:
  void genFilter();

 private:
  const FilterPolicy* filter_policy_;
  std::string keys_;                // 所有的key组合成一个大的string
  std::vector<std::size_t> start_;  // 每一个key在keys_中的偏移,可以通过start_[i
                                    // + 1] - start_[i] 计算出第i个key的长度
  std::string result_;  // 所有的filter都存放在result_中
  std::vector<uint32_t> filter_offsets_;  // 每个filter在result_中的offset
  std::vector<tools::Binary> tmp_keys_;  // 生成新的filter的时候存储临时的key
};

}  // namespace playground::cpp::misc::sst
