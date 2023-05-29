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

  virtual void create_filter(tools::Binary* keys, std::size_t n, std::string* dst) const = 0;

  [[nodiscard]] virtual const char* name() const = 0;

  [[nodiscard]] virtual bool key_may_match(const tools::Binary& key,
                                           const tools::Binary& filter) const = 0;
};

}  // namespace playground::cpp::misc::sst
