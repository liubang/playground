//=====================================================================
//
// comparator.h -
//
// Created by liubang on 2023/05/31 11:29
// Last Modified: 2023/05/31 11:29
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"

namespace playground::cpp::misc::sst {

class Comparator {
 public:
  virtual ~Comparator();

  [[nodiscard]] virtual const char* name() const = 0;

  [[nodiscard]] virtual int compare(const tools::Binary& a,
                                    const tools::Binary& b) const = 0;
};

Comparator* bytewiseComparator();

}  // namespace playground::cpp::misc::sst
