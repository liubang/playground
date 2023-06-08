//=====================================================================
//
// filter.h -
//
// Created by liubang on 2023/05/29 19:53
// Last Modified: 2023/05/29 19:53
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"

#include <string>

namespace playground::cpp::misc::sst {

class FilterPolicy {
 public:
  virtual ~FilterPolicy();

  /**
   * @brief [TODO:description]
   *
   * @param keys [TODO:parameter]
   * @param n [TODO:parameter]
   * @param dst [TODO:parameter]
   */
  virtual void createFilter(tools::Binary* keys,
                            std::size_t n,
                            std::string* dst) const = 0;

  /**
   * @brief [TODO:description]
   *
   * @return [TODO:return]
   */
  [[nodiscard]] virtual const char* name() const = 0;

  /**
   * @brief [TODO:description]
   *
   * @param key [TODO:parameter]
   * @param filter [TODO:parameter]
   * @return [TODO:return]
   */
  [[nodiscard]] virtual bool keyMayMatch(const tools::Binary& key,
                                         const tools::Binary& filter) const = 0;
};

FilterPolicy* newBloomFilterPolicy(uint64_t bits_per_key);

}  // namespace playground::cpp::misc::sst
