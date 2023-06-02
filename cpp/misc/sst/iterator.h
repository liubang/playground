//=====================================================================
//
// iterator.h -
//
// Created by liubang on 2023/06/01 19:49
// Last Modified: 2023/06/01 19:49
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"

namespace playground::cpp::misc::sst {

class Iterator {
public:
  Iterator();
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual ~Iterator() = default;

  virtual void first() = 0;
  virtual void last() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;

  [[nodiscard]] virtual bool valid() const = 0;
  [[nodiscard]] virtual tools::Binary key() const = 0;
  [[nodiscard]] virtual tools::Binary val() const = 0;
};

}  // namespace playground::cpp::misc::sst
